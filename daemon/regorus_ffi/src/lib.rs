// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// A C ABI over regorus, so the policy backend evaluates rego in-process rather than over a
// network. The base engine is loaded once with the policy and never mutated afterwards; each
// evaluation clones it, sets the input on the clone and evals, so the caller can share one base
// across threads by cloning under its own lock and letting each thread own its clone.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;

/// The rego engine one caller owns. Opaque: only the functions below reach inside it.
pub struct EngineHandle {
    engine: regorus::Engine,
}

unsafe fn borrow<'a>(handle: *const EngineHandle) -> Option<&'a EngineHandle> {
    if handle.is_null() {
        None
    } else {
        Some(&*handle)
    }
}

unsafe fn read_str<'a>(text: *const c_char) -> Option<&'a str> {
    if text.is_null() {
        return None;
    }
    CStr::from_ptr(text).to_str().ok()
}

#[no_mangle]
pub extern "C" fn daemon_regorus_new() -> *mut EngineHandle {
    Box::into_raw(Box::new(EngineHandle {
        engine: regorus::Engine::new(),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn daemon_regorus_free(handle: *mut EngineHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

/// Loads one policy module. Returns 0 on success, -1 on a parse error or bad argument.
#[no_mangle]
pub unsafe extern "C" fn daemon_regorus_add_policy(
    handle: *mut EngineHandle,
    name: *const c_char,
    rego: *const c_char,
) -> c_int {
    let handle = match handle.as_mut() {
        Some(handle) => handle,
        None => return -1,
    };
    let name = match read_str(name) {
        Some(text) => text.to_string(),
        None => return -1,
    };
    let rego = match read_str(rego) {
        Some(text) => text.to_string(),
        None => return -1,
    };
    match handle.engine.add_policy(name, rego) {
        Ok(_) => 0,
        Err(_) => -1,
    }
}

/// A clone of the base engine, for one evaluation to own. Null on a bad argument.
#[no_mangle]
pub unsafe extern "C" fn daemon_regorus_clone(handle: *const EngineHandle) -> *mut EngineHandle {
    match borrow(handle) {
        Some(handle) => Box::into_raw(Box::new(EngineHandle {
            engine: handle.engine.clone(),
        })),
        None => ptr::null_mut(),
    }
}

/// Sets @input_json as the input and evaluates @query. Returns a malloc'd C string of the first
/// expression's value as JSON (the policy decision object), or null on any error. Free it with
/// daemon_regorus_free_string.
#[no_mangle]
pub unsafe extern "C" fn daemon_regorus_eval(
    handle: *mut EngineHandle,
    input_json: *const c_char,
    query: *const c_char,
) -> *mut c_char {
    let handle = match handle.as_mut() {
        Some(handle) => handle,
        None => return ptr::null_mut(),
    };
    let input_json = match read_str(input_json) {
        Some(text) => text,
        None => return ptr::null_mut(),
    };
    let query = match read_str(query) {
        Some(text) => text.to_string(),
        None => return ptr::null_mut(),
    };

    let input = match regorus::Value::from_json_str(input_json) {
        Ok(value) => value,
        Err(_) => return ptr::null_mut(),
    };
    handle.engine.set_input(input);

    let results = match handle.engine.eval_query(query, false) {
        Ok(results) => results,
        Err(_) => return ptr::null_mut(),
    };
    let value = results
        .result
        .get(0)
        .and_then(|result| result.expressions.get(0))
        .map(|expression| expression.value.clone())
        .unwrap_or(regorus::Value::Undefined);

    let json = match serde_json::to_string(&value) {
        Ok(text) => text,
        Err(_) => return ptr::null_mut(),
    };
    match CString::new(json) {
        Ok(owned) => owned.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn daemon_regorus_free_string(text: *mut c_char) {
    if !text.is_null() {
        drop(CString::from_raw(text));
    }
}
