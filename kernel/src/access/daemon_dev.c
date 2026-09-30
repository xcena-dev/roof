// SPDX-License-Identifier: GPL-2.0-only
/*
 * daemon_dev.c - the daemon miscdevice, one per mount.
 *
 * A mount registers /dev/<daemon>-<node_id> and owns everything queued through it, so one
 * node's helper never sees another's requests. The kernel does not authenticate a helper: the
 * device's 0600 root:root mode is the whole trust boundary. One reader holds a channel at a time,
 * and seq matches a response to its request.
 */

#define pr_fmt(fmt) KBUILD_MODNAME "-" FS_DAEMON_NAME ": " fmt

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "access/acl.h"
#include "access/daemon.h"
#include "access/daemon_queue.h"
#include "core.h"
#include "test/test_hooks.h"

/* KBUILD_MODNAME, the -daemon stem and a node id, with room for the NUL. */
#define DAEMON_DEV_NAME_MAX 48

/* ── One channel's state ─────────────────────────────────────────────── */

/* The task that owns the reader slot, taken at open. A fop from any other task is rejected, and
 * the leader-based fields are what let a sibling thread through. */
struct daemon_opener {
	pid_t tgid;
	u64 self_exec_id;
	u64 start_boottime;
	kuid_t euid;
};

/* One mount's upcall channel. Refcounted because an open fd outlives the mount that registered
 * it: umount deregisters and drops its reference, and the last close frees.
 *
 * helper_dead latches on a request timeout and daemon_admit() refuses for one wait after
 * dead_since_ns. Exactly one caller then moves that mark forward and carries the probe: its answer
 * clears the latch, its timeout re-arms it. Opening or closing the device clears it too.
 */
struct daemon_channel {
	struct kref refs;
	struct miscdevice misc;
	char misc_name[DAEMON_DEV_NAME_MAX]; /* misc keeps the pointer, so the bytes live here */

	atomic_t reader_count;
	atomic_t hello_done;
	atomic_t helper_dead;
	atomic64_t asked; /* every request put on the queue, answered or not */
	atomic64_t answered; /* every answer that found its request, so a waiter can tell slow from gone */
	/* ktime of the latest timeout, and the token whose one winner carries the probe. */
	atomic64_t dead_since_ns;
	/* What the helper said it can do, from HELLO. Checked before a request only that kind of
	 * helper can answer is queued. */
	atomic64_t helper_caps;

	struct daemon_opener opener;
	struct daemon_queue queue;
};

/* misc_open leaves the miscdevice here before it calls open, and nothing overwrites it. */
static inline struct daemon_channel *daemon_channel_of(struct file *file)
{
	return container_of(file->private_data, struct daemon_channel, misc);
}

static inline bool daemon_has_cap(struct daemon_channel *channel, u64 cap)
{
	return (atomic64_read(&channel->helper_caps) & cap) != 0;
}

static inline void daemon_capture_opener(struct daemon_channel *channel)
{
	struct task_struct *leader = current->group_leader;
	channel->opener.tgid = current->tgid;
	channel->opener.self_exec_id = leader->self_exec_id;
	channel->opener.start_boottime = leader->start_boottime;
	channel->opener.euid = current_euid();
}

static inline s32 daemon_check_opener(struct daemon_channel *channel)
{
	struct task_struct *leader = current->group_leader;
	if (likely(current->tgid == channel->opener.tgid && leader->self_exec_id == channel->opener.self_exec_id &&
		   leader->start_boottime == channel->opener.start_boottime && uid_eq(current_euid(), channel->opener.euid)))
		return 0;

	return -EPERM;
}

static void daemon_release_channel(struct kref *refs)
{
	kfree(container_of(refs, struct daemon_channel, refs));
}

/* ── fops: open ─────────────────────────────────────────────────────── */

static int daemon_fops_open(struct inode *inode, struct file *file)
{
	struct daemon_channel *channel = daemon_channel_of(file);
	if (atomic_cmpxchg(&channel->reader_count, 0, 1) != 0)
		return -EBUSY;

	/* Held while the reader slot is ours, so the channel cannot go while this fd has it. */
	kref_get(&channel->refs);
	daemon_capture_opener(channel);
	atomic_set(&channel->hello_done, 0);
	atomic_set(&channel->helper_dead, 0);
	atomic64_set(&channel->answered, 0);
	atomic64_set(&channel->helper_caps, 0);
	return 0;
}

/* ── fops: release ───────────────────────────────────────────────────── */

static int daemon_fops_release(struct inode *inode, struct file *file)
{
	struct daemon_channel *channel = daemon_channel_of(file);
	atomic_set(&channel->hello_done, 0);
	memset(&channel->opener, 0, sizeof(channel->opener));
	atomic_set(&channel->reader_count, 0);
	atomic_set(&channel->helper_dead, 0);
	atomic64_set(&channel->helper_caps, 0);

	daemon_queue_purge(&channel->queue, -ENOSYS);

	kref_put(&channel->refs, daemon_release_channel);
	return 0;
}

/* ── fops: read (blocking dequeue) ──────────────────────────────────── */

/* One request per read, returning exactly sizeof(fs_daemon_hdr) + payload. */
static ssize_t daemon_fops_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	if (!buf)
		return -EINVAL;

	struct daemon_channel *channel = daemon_channel_of(file);
	s32 ret = daemon_check_opener(channel);
	if (ret)
		return ret;

	/* No HELLO gate here: the helper's reader thread starts before the handshake, and nothing
	 * reaches the queue before HELLO, so the read just blocks. */
	if (file->f_flags & O_NONBLOCK) {
		if (!daemon_queue_has_outbound(&channel->queue))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(channel->queue.read_wq, daemon_queue_has_outbound(&channel->queue));
		if (ret)
			return ret;
	}

	/* Read before the dequeue names a request, since what this stamps is how long the helper's
	 * reader took to run once there was one for it. */
	const u64 woke_ns = ktime_get_ns();

	struct daemon_pending *pending = daemon_queue_dequeue_outbound(&channel->queue);
	if (!pending)
		return -EAGAIN; /* spurious wakeup */
	test_stamp_upcall(pending->req_frame.hdr.seq, TEST_UPCALL_READ_WOKE, woke_ns);

	if (!daemon_check_type(pending->type)) {
		/* Do not requeue a corrupt slot: it would loop on the same one forever. The waiting
		 * caller times out and frees it. */
		pr_err("bad slot type %d seq=%llu\n", pending->type, pending->req_frame.hdr.seq);
		daemon_pending_put(pending);
		return -EPROTO;
	}

	/* Pre-built at enqueue, so the helper sees hdr+payload from one copy_to_user. */
	u32 total = sizeof(struct fs_daemon_hdr) + daemon_get_req_size(pending->type);
	if (count < total) {
		/* Requeue so the helper can retry with a buffer that fits. */
		ret = -EINVAL;
		goto out_requeue;
	}
	if (copy_to_user(buf, &pending->req_frame, total)) {
		ret = -EFAULT;
		goto out_requeue;
	}

	const u64 read_done_ns = ktime_get_ns();
	test_stamp_upcall(pending->req_frame.hdr.seq, TEST_UPCALL_READ_DONE, read_done_ns);
	if (!daemon_expects_reply(pending->type)) {
		/* Nothing comes back, so the helper holding the request is where the trip ends and
		 * the sample folds. */
		test_stamp_upcall(pending->req_frame.hdr.seq, TEST_UPCALL_RESUMED, read_done_ns);
		daemon_queue_retire(&channel->queue, pending);
	}
	daemon_pending_put(pending);
	return total;

out_requeue:
	daemon_queue_requeue_outbound(&channel->queue, pending);
	daemon_pending_put(pending);
	return ret;
}

/* ── fops: poll ─────────────────────────────────────────────────────── */

/* Lets the helper wait for a request inside its own event loop instead of on a thread parked in
 * read(). Without this the VFS reports the fd always readable and epoll refuses it. */
static __poll_t daemon_fops_poll(struct file *file, struct poll_table_struct *wait)
{
	struct daemon_channel *channel = daemon_channel_of(file);
	if (daemon_check_opener(channel))
		return EPOLLERR;

	poll_wait(file, &channel->queue.read_wq, wait);

	__poll_t events = EPOLLOUT | EPOLLWRNORM;
	if (daemon_queue_has_outbound(&channel->queue))
		events |= EPOLLIN | EPOLLRDNORM;

	return events;
}

/* ── fops: write (response from helper) ─────────────────────────────── */

/* Copy the @want payload bytes after @hdr, once both hdr->payload_len and @count are known to
 * cover them. Excess is ignored: payload_len is capped upstream. */
static inline s32 daemon_copy_payload(const struct fs_daemon_hdr *hdr, const char __user *buf, u32 count, void *dst,
				      u32 want)
{
	if (hdr->payload_len < want)
		return -EINVAL;
	if (count < sizeof(*hdr) + want)
		return -EINVAL;
	if (copy_from_user(dst, buf + sizeof(*hdr), want))
		return -EFAULT;
	return 0;
}

static s32 daemon_handle_hello(struct daemon_channel *channel, const struct fs_daemon_hdr *hdr,
			       const char __user *buf, u32 count)
{
	if (atomic_cmpxchg(&channel->hello_done, 0, 0) != 0)
		return -EPROTO; /* duplicate HELLO */

	struct fs_daemon_hello hello;
	s32 err = daemon_copy_payload(hdr, buf, count, &hello, sizeof(hello));
	if (err)
		return err;

	if (hello.protocol_version < FS_DAEMON_PROTOCOL_MIN ||
	    hello.protocol_version > FS_DAEMON_PROTOCOL_MAX)
		return -EPROTO;

	/* Before hello_done, so a request gated on a capability cannot pass the gate and then find
	 * the mask still empty. */
	atomic64_set(&channel->helper_caps, hello.capabilities);
	atomic_set(&channel->hello_done, 1);
	pr_info("%s: helper connected (pid=%u caps=0x%llx)\n",
		channel->misc_name, hello.helper_pid, hello.capabilities);

	return (s32)(sizeof(*hdr) + sizeof(hello));
}

static s32 daemon_handle_response(struct daemon_channel *channel, const struct fs_daemon_hdr *hdr,
				  const char __user *buf, u32 count, enum daemon_req_type rtype)
{
	if (!atomic_read(&channel->hello_done))
		return -EPROTO;

	u32 resp_size = daemon_get_resp_size(rtype);
	union daemon_resp_payload resp = {};
	s32 err = daemon_copy_payload(hdr, buf, count, &resp, resp_size);
	if (err)
		return err;

	err = daemon_queue_dispatch_response(&channel->queue, hdr->seq, rtype, &resp);
	if (err == 0)
		atomic64_inc(&channel->answered);
	if (err == -ENOENT) {
		/* Late response — request already timed out, ignore */
		pr_debug("late RESPONSE type=%u seq=%llu ignored\n", hdr->type, hdr->seq);
		err = 0;
	}

	/* A well-formed answer, late or not, is a helper that is serving again. The latch was for a
	 * helper that had gone quiet, and a slow one that keeps timing out re-latches on its own. */
	if (!err && atomic_xchg(&channel->helper_dead, 0))
		pr_info("%s: helper answered again — serving\n", channel->misc_name);

	return err ? err : (s32)(sizeof(*hdr) + resp_size);
}

static ssize_t daemon_fops_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct daemon_channel *channel = daemon_channel_of(file);
	s32 ret = daemon_check_opener(channel);
	if (ret)
		return ret;

	if (!buf)
		return -EINVAL;
	/* Reject pathologically large writes early to bound copy_from_user */
	if (count > FS_DAEMON_MAX_WRITE_BYTES)
		return -EMSGSIZE;
	/* fd must be opened (reader) — defends against UAF if release races */
	if (!atomic_read(&channel->reader_count))
		return -ENOSYS;

	struct fs_daemon_hdr hdr;
	if (count < sizeof(hdr))
		return -EINVAL;
	if (copy_from_user(&hdr, buf, sizeof(hdr)))
		return -EFAULT;
	if (hdr.magic != FS_DAEMON_MAGIC || hdr.version < FS_DAEMON_PROTOCOL_MIN || hdr.version > FS_DAEMON_PROTOCOL_MAX)
		return -EPROTO;
	if (hdr.payload_len > FS_DAEMON_MAX_PAYLOAD_BYTES)
		return -EMSGSIZE;
	if (hdr.payload_len > count - sizeof(hdr))
		return -EINVAL;

	switch (hdr.type) {
	case FS_DAEMON_MSG_HELLO:
		return daemon_handle_hello(channel, &hdr, buf, count);
	case FS_DAEMON_MSG_ACCESS_RESPONSE:
		return daemon_handle_response(channel, &hdr, buf, count, DAEMON_REQ_ACCESS);
	case FS_DAEMON_MSG_ATTEST_RESPONSE:
		return daemon_handle_response(channel, &hdr, buf, count, DAEMON_REQ_ATTEST);
	case FS_DAEMON_MSG_LOCK_RESPONSE:
		return daemon_handle_response(channel, &hdr, buf, count, DAEMON_REQ_LOCK);
	default:
		return -EPROTO;
	}
}

/* ── Miscdevice registration ─────────────────────────────────────────── */

static const struct file_operations daemon_fops = {
	.owner = THIS_MODULE,
	.open = daemon_fops_open,
	.release = daemon_fops_release,
	.read = daemon_fops_read,
	.write = daemon_fops_write,
	.poll = daemon_fops_poll,
	.llseek = noop_llseek,
};

/* ── Per-mount lifecycle ─────────────────────────────────────────────── */

s32 daemon_create_channel(struct fs_sb_info *sbi)
{
	if (!sbi)
		return -EINVAL;

	struct daemon_channel *channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return -ENOMEM;

	kref_init(&channel->refs);
	daemon_queue_init(&channel->queue);
	snprintf(channel->misc_name, sizeof(channel->misc_name), "%s-%u", FS_DAEMON_NAME, sbi->node_id);
	channel->misc.minor = MISC_DYNAMIC_MINOR;
	channel->misc.name = channel->misc_name;
	channel->misc.fops = &daemon_fops;
	channel->misc.mode = 0600;

	s32 ret = misc_register(&channel->misc);
	if (ret) {
		pr_err("failed to register /dev/%s: %d\n", channel->misc_name, ret);
		kfree(channel);
		return ret;
	}

	sbi->daemon = channel;
	pr_info("registered /dev/%s (minor=%d)\n", channel->misc_name, channel->misc.minor);

	return 0;
}

void daemon_delete_channel(struct fs_sb_info *sbi)
{
	if (!sbi || !sbi->daemon)
		return;

	struct daemon_channel *channel = sbi->daemon;
	sbi->daemon = NULL;

	/* Deregistered first, so nothing new opens the channel while its slots are being failed. */
	misc_deregister(&channel->misc);
	daemon_queue_purge(&channel->queue, -ENOSYS);
	pr_info("deregistered /dev/%s\n", channel->misc_name);
	kref_put(&channel->refs, daemon_release_channel);
}

/* ── Public request API ──────────────────────────────────────────────── */

void daemon_fill_task(enum daemon_req_type type, union daemon_req_payload *req)
{
	if (!req)
		return;

	struct acl_caller_identity caller;
	acl_read_caller_identity(&caller);

	switch (type) {
	case DAEMON_REQ_ACCESS:
		req->access.consumer_pid = caller.tgid;
		req->access.consumer_uid = caller.uid;
		req->access.consumer_gid = caller.gid;
		req->access.consumer_start_boottime_ns = caller.birth_time;
		req->access.consumer_exe_inode_ino = caller.exe_inode_ino;
		req->access.consumer_exe_inode_dev = caller.exe_inode_dev;
		acl_get_exe_path(req->access.consumer_exe_path, sizeof(req->access.consumer_exe_path));
		break;

	case DAEMON_REQ_ATTEST:
		req->attest.owner_pid = caller.tgid;
		req->attest.owner_uid = caller.uid;
		req->attest.owner_gid = caller.gid;
		req->attest.owner_start_boottime_ns = caller.birth_time;
		req->attest.owner_exe_inode_ino = caller.exe_inode_ino;
		req->attest.owner_exe_inode_dev = caller.exe_inode_dev;
		acl_get_exe_path(req->attest.owner_exe_path, sizeof(req->attest.owner_exe_path));
		break;

	case DAEMON_REQ_LOCK:
	case DAEMON_REQ_UNLOCK:
		/* A lock belongs to the mount, not to whoever happened to trip the path, so these
		 * carry a domain name and no task identity. */
		break;
	}
}

/* The HELLO capability a helper must carry to be sent this kind of request. 0 for a tag that names
 * none, which admission reads as a request nobody can answer. */
static u64 daemon_get_required_cap(enum daemon_req_type type)
{
	switch (type) {
	case DAEMON_REQ_ACCESS:
		return FS_DAEMON_CAP_ACCESS_REQUEST;
	case DAEMON_REQ_ATTEST:
		return FS_DAEMON_CAP_ATTEST_OWNER;
	case DAEMON_REQ_LOCK:
	case DAEMON_REQ_UNLOCK:
		return FS_DAEMON_CAP_LOCK;
	}
	return 0;
}

/* What every upcall passes before it is queued: a connected helper that declared this kind of
 * request and is not latched dead. On success @timeout_ms is what one wait here is allowed. */
static s32 daemon_admit(struct fs_sb_info *sbi, enum daemon_req_type type,
			struct daemon_channel **channel_out, u32 *timeout_ms)
{
	if (fs_is_fenced(sbi) && daemon_expects_reply(type))
		return -EIO;

	struct daemon_channel *channel = sbi->daemon;
	if (!channel ||
	    !atomic_read(&channel->reader_count) ||
	    !atomic_read(&channel->hello_done))
		return -ENOSYS;

	/* HELLO is what the helper answers, so a frame outside it would sit on the queue until the
	 * wait ran out. Refused here instead, and before the latch, which spends no probe on it. */
	const u64 needed = daemon_get_required_cap(type);
	if (needed == 0 || !daemon_has_cap(channel, needed))
		return -EOPNOTSUPP;

	/* Snapshot once: the wait must not see the value change under it. */
	u32 allowed_ms = test_daemon_timeout_ms(FS_DAEMON_DEFAULT_TIMEOUT_MS);
	if (allowed_ms > FS_DAEMON_MAX_TIMEOUT_MS)
		allowed_ms = FS_DAEMON_DEFAULT_TIMEOUT_MS;

	/* A quiet helper would cost every caller a full wait. Only a request let through finds out
	 * that it is back, so moving the mark is what picks the single caller carrying that probe. */
	if (atomic_read(&channel->helper_dead)) {
		const s64 since = atomic64_read(&channel->dead_since_ns);
		const s64 now = (s64)ktime_get_ns();

		if (now < since + (s64)allowed_ms * NSEC_PER_MSEC)
			return -ETIMEDOUT;
		if (atomic64_cmpxchg(&channel->dead_since_ns, since, now) != since)
			return -ETIMEDOUT;
	}

	*channel_out = channel;
	*timeout_ms = allowed_ms;
	return 0;
}

/* Admit and queue @req. On success *@pending_out is the slot daemon_collect waits on, and
 * *@timeout_ms how long that wait may take. */
static s32 daemon_submit(struct fs_sb_info *sbi, enum daemon_req_type type, const union daemon_req_payload *req,
			 struct daemon_pending **pending_out, u32 *timeout_ms)
{
	struct daemon_channel *channel;
	s32 ret = daemon_admit(sbi, type, &channel, timeout_ms);
	if (ret)
		return ret;

	struct daemon_pending *pending = daemon_queue_enqueue(&channel->queue, type, req);
	if (IS_ERR(pending))
		return PTR_ERR(pending);

	*pending_out = pending;
	return 0;
}

/*
 * Wait for the answer to a submit. A wait that runs out latches the channel dead only when the
 * helper answered nobody else meanwhile: a helper working through a long queue is slow and not
 * gone, and latching on it would refuse every caller for a wait. This caller still gets its timeout.
 */
static s32 daemon_collect(struct fs_sb_info *sbi, struct daemon_pending *pending, union daemon_resp_payload *resp,
			  u32 timeout_ms, s64 answered_before)
{
	struct daemon_channel *channel = sbi->daemon;
	s32 ret = daemon_queue_wait(&channel->queue, pending, resp, timeout_ms);
	if (ret == -ETIMEDOUT) {
		if (atomic64_read(&channel->answered) != answered_before) {
			pr_warn_ratelimited("%s: helper response timeout while it answered others — slow, not gone\n",
					    channel->misc_name);
			return ret;
		}
		atomic64_set(&channel->dead_since_ns, (s64)ktime_get_ns());
		if (atomic_xchg(&channel->helper_dead, 1) == 0)
			pr_err("%s: helper response timeout — fail-closed\n", channel->misc_name);
	}
	return ret;
}

s32 daemon_request(struct fs_sb_info *sbi, enum daemon_req_type type, const union daemon_req_payload *req,
		   union daemon_resp_payload *resp)
{
	if (!sbi || !req || !resp || !daemon_check_type(type) || !daemon_expects_reply(type))
		return -EINVAL;

	atomic64_inc(&sbi->daemon->asked);
	const s64 answered_before = atomic64_read(&sbi->daemon->answered);

	struct daemon_pending *pending;
	u32 timeout_ms;
	s32 ret = daemon_submit(sbi, type, req, &pending, &timeout_ms);
	return ret ? ret : daemon_collect(sbi, pending, resp, timeout_ms, answered_before);
}

void daemon_drop_waiters(struct fs_sb_info *sbi)
{
	struct daemon_channel *channel = sbi ? sbi->daemon : NULL;
	if (channel)
		daemon_queue_fence(&channel->queue);
}

/* Queue a request that has no answer and return once the helper is woken. The slot is the read
 * path's to finish, so the caller never touches it. */
static s32 daemon_notify(struct fs_sb_info *sbi, enum daemon_req_type type, const union daemon_req_payload *req)
{
	if (!sbi || !req || !daemon_check_type(type) || daemon_expects_reply(type))
		return -EINVAL;

	struct daemon_pending *pending;
	u32 timeout_ms;
	return daemon_submit(sbi, type, req, &pending, &timeout_ms);
}

/* Copy @domain into @dst NUL-terminated, refusing a name that does not fit rather than cutting
 * it: a cut name would take a lock on some other domain. */
static s32 daemon_copy_domain(char *dst, u32 size, const char *domain)
{
	if (!domain)
		return -EINVAL;

	u32 len = strnlen(domain, size);
	if (len == 0)
		return -EINVAL;
	if (len >= size)
		return -ENAMETOOLONG;

	memcpy(dst, domain, len);
	memset(dst + len, 0, size - len);

	return 0;
}

s32 daemon_lock_domain_begin(struct fs_sb_info *sbi, const char *domain, u32 timeout_ms,
			     struct daemon_lock_wait *wait)
{
	if (!wait)
		return -EINVAL;
	wait->pending = NULL;
	wait->domain = domain;
	if (!sbi || !sbi->daemon)
		return -ENOSYS;

	union daemon_req_payload req = {};
	s32 ret = daemon_copy_domain(req.lock.domain, sizeof(req.lock.domain), domain);
	if (ret)
		return ret;

	if (timeout_ms == 0 || timeout_ms > FS_DAEMON_LOCK_MAX_WAIT_MS)
		timeout_ms = FS_DAEMON_LOCK_MAX_WAIT_MS;
	req.lock.timeout_ms = timeout_ms;

	wait->answered_before = atomic64_read(&sbi->daemon->answered);
	return daemon_submit(sbi, DAEMON_REQ_LOCK, &req, &wait->pending, &wait->timeout_ms);
}

s32 daemon_lock_domain_finish(struct fs_sb_info *sbi, struct daemon_lock_wait *wait)
{
	if (!sbi || !wait || !wait->pending)
		return -EINVAL;

	union daemon_resp_payload resp = {};
	s32 ret = daemon_collect(sbi, wait->pending, &resp, wait->timeout_ms, wait->answered_before);
	wait->pending = NULL;
	if (ret) {
		/* The helper may still grant a wait nobody is left to take, and that turn would then
		 * have no owner to give it back. A domain it does not hold answers 0. */
		daemon_unlock_domain(sbi, wait->domain);
		return ret;
	}
	return resp.lock.status;
}

s32 daemon_lock_domain(struct fs_sb_info *sbi, const char *domain, u32 timeout_ms)
{
	struct daemon_lock_wait wait;
	s32 ret = daemon_lock_domain_begin(sbi, domain, timeout_ms, &wait);
	return ret ? ret : daemon_lock_domain_finish(sbi, &wait);
}

s32 daemon_unlock_domain(struct fs_sb_info *sbi, const char *domain)
{
	if (!sbi || !sbi->daemon)
		return -ENOSYS;

	union daemon_req_payload req = {};
	s32 ret = daemon_copy_domain(req.unlock.domain, sizeof(req.unlock.domain), domain);
	if (ret)
		return ret;

	/* The helper serves frames in order, so a LOCK queued after this one is served after the
	 * release whether or not anyone waited for it. */
	ret = daemon_notify(sbi, DAEMON_REQ_UNLOCK, &req);
	if (ret)
		pr_warn_ratelimited("%s: UNLOCK %s not queued (%d), the turn stays held\n",
				    sbi->daemon->misc_name, domain, ret);
	return ret;
}

u32 daemon_get_queue_depth(struct fs_sb_info *sbi)
{
	if (!sbi || !sbi->daemon)
		return 0;
	return daemon_queue_get_depth(&sbi->daemon->queue);
}

void daemon_get_state(struct fs_sb_info *sbi, struct daemon_state *out)
{
	if (!out || !sbi || !sbi->daemon)
		return;

	memset(out, 0, sizeof(*out));

	struct daemon_channel *channel = sbi->daemon;
	out->reader_open = atomic_read(&channel->reader_count) != 0;
	out->hello_done = atomic_read(&channel->hello_done) != 0;
	out->helper_dead = atomic_read(&channel->helper_dead) != 0;
	out->queue_depth = daemon_queue_get_depth(&channel->queue);
	out->asked = (u64)atomic64_read(&channel->asked);
}
