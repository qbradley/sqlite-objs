//! Ergonomic wrappers for sqlite-objs VFS file-control operations.
//!
//! These functions call the custom FCNTL opcodes exposed by the sqlite-objs
//! VFS through [`rusqlite::Connection`].  They require the connection to be
//! opened on the `"sqlite-objs"` VFS; calling them on a plain SQLite
//! connection will return [`SqliteObjsError::Sqlite`].
//!
//! # Example
//!
//! ```no_run
//! use rusqlite::Connection;
//! use sqlite_objs::pragmas;
//!
//! # fn example() -> sqlite_objs::Result<()> {
//! // Assume VFS is registered and connection is open
//! let conn = Connection::open_with_flags_and_vfs(
//!     "mydb.db",
//!     rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE
//!         | rusqlite::OpenFlags::SQLITE_OPEN_CREATE,
//!     "sqlite-objs",
//! ).expect("open");
//!
//! let metrics = pragmas::get_stats(&conn)?;
//! println!("cache hits: {}", metrics.cache_hits);
//!
//! pragmas::reset_stats(&conn)?;
//!
//! let downloads = pragmas::get_download_count(&conn)?;
//! println!("blob downloads: {downloads}");
//! # Ok(())
//! # }
//! ```

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;

use rusqlite::Connection;

use crate::metrics::VfsMetrics;
use crate::{Result, SqliteObjsError};

/// Name of the main database schema passed to `sqlite3_file_control`.
const MAIN_DB: &CStr = c"main";

/// Retrieve all VFS activity metrics for the connection.
///
/// Calls FCNTL opcode 201 (`SQLITE_OBJS_FCNTL_STATS`) which returns a
/// `sqlite3_malloc`-allocated `key=value\n` string.  The string is parsed
/// into a [`VfsMetrics`] struct and the C memory is freed before returning.
///
/// # Errors
///
/// - [`SqliteObjsError::Sqlite`] if the file-control call fails (e.g. the
///   connection does not use the sqlite-objs VFS).
/// - [`SqliteObjsError::MetricsParse`] if the returned text cannot be parsed.
pub fn get_stats(conn: &Connection) -> Result<VfsMetrics> {
    let mut stats_ptr: *mut c_char = ptr::null_mut();

    // SAFETY: We pass a valid db handle obtained from rusqlite, a valid
    // C-string schema name, the correct FCNTL opcode, and a pointer to our
    // local `stats_ptr` which the VFS writes into.
    let rc = unsafe {
        rusqlite::ffi::sqlite3_file_control(
            conn.handle(),
            MAIN_DB.as_ptr(),
            sqlite_objs_sys::SQLITE_OBJS_FCNTL_STATS,
            (&raw mut stats_ptr).cast(),
        )
    };

    if rc != sqlite_objs_sys::SQLITE_OK {
        return Err(SqliteObjsError::Sqlite(rc));
    }

    if stats_ptr.is_null() {
        return Err(SqliteObjsError::Sqlite(sqlite_objs_sys::SQLITE_NOMEM));
    }

    // SAFETY: The VFS allocated this string with sqlite3_malloc and
    // guaranteed it is NUL-terminated.  We copy it into a Rust String
    // and free the C allocation immediately.
    let text = unsafe {
        let cstr = CStr::from_ptr(stats_ptr);
        let owned = cstr.to_string_lossy().into_owned();
        rusqlite::ffi::sqlite3_free(stats_ptr.cast());
        owned
    };

    VfsMetrics::parse(&text).map_err(|e| SqliteObjsError::MetricsParse(e.message))
}

/// Reset all VFS activity metrics counters to zero.
///
/// Calls FCNTL opcode 202 (`SQLITE_OBJS_FCNTL_STATS_RESET`).
///
/// # Errors
///
/// Returns [`SqliteObjsError::Sqlite`] if the file-control call fails.
pub fn reset_stats(conn: &Connection) -> Result<()> {
    // SAFETY: The STATS_RESET opcode ignores the arg pointer; we pass null.
    let rc = unsafe {
        rusqlite::ffi::sqlite3_file_control(
            conn.handle(),
            MAIN_DB.as_ptr(),
            sqlite_objs_sys::SQLITE_OBJS_FCNTL_STATS_RESET,
            ptr::null_mut(),
        )
    };

    if rc != sqlite_objs_sys::SQLITE_OK {
        return Err(SqliteObjsError::Sqlite(rc));
    }

    Ok(())
}

/// Get the number of full blob downloads performed on the main database file.
///
/// Calls FCNTL opcode 200 (`SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT`).
/// ETag cache hits do not increment this counter.
///
/// # Errors
///
/// Returns [`SqliteObjsError::Sqlite`] if the file-control call fails.
pub fn get_download_count(conn: &Connection) -> Result<i32> {
    let mut count: i32 = 0;

    // SAFETY: The DOWNLOAD_COUNT opcode writes an int through the arg pointer.
    let rc = unsafe {
        rusqlite::ffi::sqlite3_file_control(
            conn.handle(),
            MAIN_DB.as_ptr(),
            sqlite_objs_sys::SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT,
            (&raw mut count).cast(),
        )
    };

    if rc != sqlite_objs_sys::SQLITE_OK {
        return Err(SqliteObjsError::Sqlite(rc));
    }

    Ok(count)
}
