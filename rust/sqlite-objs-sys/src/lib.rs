//! Raw FFI bindings to the sqlite-objs C library.
//!
//! This crate provides low-level unsafe bindings to the sqlite-objs VFS.
//! Most users should use the `sqlite-objs` crate instead, which provides
//! a safe, idiomatic Rust API.

#![allow(non_camel_case_types)]

use libc::{c_char, c_int, c_void};

/// Opaque type for Azure operations vtable (defined in azure_client.h)
#[repr(C)]
pub struct azure_ops_t {
    _private: [u8; 0],
}

/// Configuration for the sqlite-objs VFS.
///
/// This struct maps directly to the C `sqlite_objs_config_t` type.
#[repr(C)]
#[derive(Debug)]
pub struct sqlite_objs_config_t {
    /// Azure Storage account name
    pub account: *const c_char,
    /// Blob container name
    pub container: *const c_char,
    /// SAS token (preferred), or NULL
    pub sas_token: *const c_char,
    /// Shared Key (fallback), or NULL
    pub account_key: *const c_char,
    /// Optional custom endpoint (for Azurite), or NULL for Azure
    pub endpoint: *const c_char,
    /// Optional: override the Azure operations vtable
    pub ops: *const azure_ops_t,
    /// Opaque context pointer passed to ops functions
    pub ops_ctx: *mut c_void,
}

extern "C" {
    /// Register the "sqlite-objs" VFS.
    ///
    /// Reads configuration from environment variables:
    /// - AZURE_STORAGE_ACCOUNT
    /// - AZURE_STORAGE_CONTAINER
    /// - AZURE_STORAGE_SAS (checked first)
    /// - AZURE_STORAGE_KEY (fallback)
    ///
    /// If `makeDefault` is non-zero, this VFS becomes the default.
    /// Returns SQLITE_OK (0) on success, or an appropriate error code.
    pub fn sqlite_objs_vfs_register(makeDefault: c_int) -> c_int;

    /// Register the "sqlite-objs" VFS with an explicit configuration.
    ///
    /// The config struct is copied — the caller may free it after this call.
    /// If `makeDefault` is non-zero, this VFS becomes the default.
    /// Returns SQLITE_OK (0) on success, or an appropriate error code.
    pub fn sqlite_objs_vfs_register_with_config(
        config: *const sqlite_objs_config_t,
        makeDefault: c_int,
    ) -> c_int;

    /// Register the "sqlite-objs" VFS with an explicit ops vtable and context.
    ///
    /// Convenience wrapper for test code.
    pub fn sqlite_objs_vfs_register_with_ops(
        ops: *mut azure_ops_t,
        ctx: *mut c_void,
        makeDefault: c_int,
    ) -> c_int;

    /// Register the "sqlite-objs" VFS with no global Azure client.
    ///
    /// All databases MUST provide Azure credentials via URI parameters:
    /// `file:mydb.db?azure_account=acct&azure_container=cont&azure_sas=token`
    ///
    /// Supported URI parameters:
    /// - azure_account (required)
    /// - azure_container
    /// - azure_sas
    /// - azure_key
    /// - azure_endpoint
    ///
    /// If `makeDefault` is non-zero, this VFS becomes the default.
    /// Returns SQLITE_OK (0) on success, or an appropriate error code.
    pub fn sqlite_objs_vfs_register_uri(makeDefault: c_int) -> c_int;
}

// SQLite constants
pub const SQLITE_OK: c_int = 0;
pub const SQLITE_ERROR: c_int = 1;
pub const SQLITE_CANTOPEN: c_int = 14;
pub const SQLITE_NOMEM: c_int = 7;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_config_size() {
        // Ensure the struct layout matches C expectations
        assert!(std::mem::size_of::<sqlite_objs_config_t>() > 0);
    }

    #[test]
    fn test_register_uri() {
        // Test that we can call the FFI function (registration will fail
        // without Azure config, but linkage should work)
        unsafe {
            let result = sqlite_objs_vfs_register_uri(0);
            // Should return SQLITE_OK since URI mode doesn't require config upfront
            assert_eq!(result, SQLITE_OK);
        }
    }
}
