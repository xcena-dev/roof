// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// ordered.hpp -- an atomic whose memory order its type fixes.
//
// The order belongs to the protocol a word takes part in, so it is chosen once where the word is
// declared instead of at every load and store. Each type holds one std::atomic<T> and nothing else,
// so it keeps that atomic's size and alignment and can sit in memory another process maps.

#pragma once

#include <atomic>

namespace ordered
{

// A word nothing is handed over through: a flag, a count, a value read on its own.
template <typename T>
class Relaxed
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Zero, so an array of words, which builds each element by default, starts known.
    constexpr Relaxed() noexcept
        : value_{T{}}
    {
    }

    constexpr explicit Relaxed(T initial) noexcept
        : value_{initial}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    [[nodiscard]] T load() const noexcept
    {
        return value_.load(std::memory_order_relaxed);
    }

    void store(T value) noexcept
    {
        value_.store(value, std::memory_order_relaxed);
    }

    // Each returns the value before the change, as std::atomic does.
    T add(T delta) noexcept
    {
        return value_.fetch_add(delta, std::memory_order_relaxed);
    }

    T subtract(T delta) noexcept
    {
        return value_.fetch_sub(delta, std::memory_order_relaxed);
    }

    T setBits(T bits) noexcept
    {
        return value_.fetch_or(bits, std::memory_order_relaxed);
    }

    T clearBits(T bits) noexcept
    {
        return value_.fetch_and(~bits, std::memory_order_relaxed);
    }

private:
    std::atomic<T> value_;
};

// A word that hands over what was written before its store to whoever loads it. reset() is for the
// start of a round, when there is nothing to hand over yet.
template <typename T>
class Published
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Zero, so an array of words, which builds each element by default, starts known.
    constexpr Published() noexcept
        : value_{T{}}
    {
    }

    constexpr explicit Published(T initial) noexcept
        : value_{initial}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    [[nodiscard]] T load() const noexcept
    {
        return value_.load(std::memory_order_acquire);
    }

    void store(T value) noexcept
    {
        value_.store(value, std::memory_order_release);
    }

    void reset(T value) noexcept
    {
        value_.store(value, std::memory_order_relaxed);
    }

    // A read-modify-write takes what the last store handed over and hands over what came before it,
    // so each is acq_rel. Each returns the value before the change, as std::atomic does.
    T add(T delta) noexcept
    {
        return value_.fetch_add(delta, std::memory_order_acq_rel);
    }

    T subtract(T delta) noexcept
    {
        return value_.fetch_sub(delta, std::memory_order_acq_rel);
    }

    T setBits(T bits) noexcept
    {
        return value_.fetch_or(bits, std::memory_order_acq_rel);
    }

    T clearBits(T bits) noexcept
    {
        return value_.fetch_and(~bits, std::memory_order_acq_rel);
    }

    T exchange(T value) noexcept
    {
        return value_.exchange(value, std::memory_order_acq_rel);
    }

    // Compare-and-swap, strong, so a single try never fails without a reason. On failure @expected
    // takes the value found, read with acquire.
    [[nodiscard]] bool cas(T& expected, T desired) noexcept
    {
        return value_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

private:
    std::atomic<T> value_;
};

// A word whose protocol needs one order that every thread agrees on, such as a store that has to be
// seen before a later load of another word. Every access is seq_cst.
template <typename T>
class SeqCst
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Zero, so an array of words, which builds each element by default, starts known.
    constexpr SeqCst() noexcept
        : value_{T{}}
    {
    }

    constexpr explicit SeqCst(T initial) noexcept
        : value_{initial}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    [[nodiscard]] T load() const noexcept
    {
        return value_.load(std::memory_order_seq_cst);
    }

    void store(T value) noexcept
    {
        value_.store(value, std::memory_order_seq_cst);
    }

    // Each returns the value before the change, as std::atomic does.
    T add(T delta) noexcept
    {
        return value_.fetch_add(delta, std::memory_order_seq_cst);
    }

    T subtract(T delta) noexcept
    {
        return value_.fetch_sub(delta, std::memory_order_seq_cst);
    }

    T setBits(T bits) noexcept
    {
        return value_.fetch_or(bits, std::memory_order_seq_cst);
    }

    T clearBits(T bits) noexcept
    {
        return value_.fetch_and(~bits, std::memory_order_seq_cst);
    }

    T exchange(T value) noexcept
    {
        return value_.exchange(value, std::memory_order_seq_cst);
    }

    // Compare-and-swap, strong. On failure @expected takes the value found.
    [[nodiscard]] bool cas(T& expected, T desired) noexcept
    {
        return value_.compare_exchange_strong(expected, desired, std::memory_order_seq_cst);
    }

private:
    std::atomic<T> value_;
};

}  // namespace ordered
