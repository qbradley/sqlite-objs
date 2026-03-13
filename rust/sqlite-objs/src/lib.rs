//! # sqlite-objs - SQLite VFS backed by Azure Blob Storage
//!
//! This crate provides safe Rust bindings to sqlite-objs, a SQLite VFS (Virtual File System)
//! that stores database files in Azure Blob Storage.
//!
//! ## Features
//!
//! - Store SQLite databases in Azure Blob Storage (page blobs for DB, block blobs for journal)
//! - Blob lease-based locking for safe concurrent access
//! - Full-blob caching for performance
//! - SAS token and Shared Key authentication
//! - URI-based per-database configuration
//!
//! ## Usage
//!
//! ### Basic Registration (Environment Variables)
//!
//! ```no_run
//! use sqlite_objs::SqliteObjsVfs;
//! use rusqlite::Connection;
//!
//! // Register VFS from environment variables
//! SqliteObjsVfs::register(false)?;
//!
//! // Open a database using the sqlite-objs VFS
//! let conn = Connection::open_with_flags_and_vfs(
//!     "mydb.db",
//!     rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_CREATE,
//!     "sqlite-objs"
//! )?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! ### URI Mode (Per-Database Credentials)
//!
//! ```no_run
//! use sqlite_objs::SqliteObjsVfs;
//! use rusqlite::Connection;
//!
//! // Register VFS in URI mode (no global config)
//! SqliteObjsVfs::register_uri(false)?;
//!
//! // Open database with Azure credentials in URI
//! let conn = Connection::open_with_flags_and_vfs(
//!     "file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv=2024...",
//!     rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_CREATE | rusqlite::OpenFlags::SQLITE_OPEN_URI,
//!     "sqlite-objs"
//! )?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! ### Explicit Configuration
//!
//! ```no_run
//! use sqlite_objs::{SqliteObjsVfs, SqliteObjsConfig};
//! use rusqlite::Connection;
//!
//! let config = SqliteObjsConfig {
//!     account: "myaccount".to_string(),
//!     container: "databases".to_string(),
//!     sas_token: Some("sv=2024-08-04&...".to_string()),
//!     account_key: None,
//!     endpoint: None,
//! };
//!
//! SqliteObjsVfs::register_with_config(&config, false)?;
//!
//! let conn = Connection::open_with_flags_and_vfs(
//!     "mydb.db",
//!     rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_CREATE,
//!     "sqlite-objs"
//! )?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```

use std::ffi::CString;
use std::ptr;
use thiserror::Error;

/// Error type for sqlite-objs operations.
#[derive(Error, Debug)]
pub enum SqliteObjsError {
    /// SQLite returned an error code
    #[error("SQLite error: {0}")]
    Sqlite(i32),

    /// Invalid configuration (e.g., null bytes in strings)
    #[error("Invalid configuration: {0}")]
    InvalidConfig(String),

    /// VFS registration failed
    #[error("VFS registration failed: {0}")]
    RegistrationFailed(String),
}

/// Result type for sqlite-objs operations.
pub type Result<T> = std::result::Result<T, SqliteObjsError>;

/// Configuration for the sqlite-objs VFS.
///
/// Maps to the C `sqlite_objs_config_t` struct. All fields are owned strings
/// for safety and convenience.
#[derive(Debug, Clone)]
pub struct SqliteObjsConfig {
    /// Azure Storage account name
    pub account: String,
    /// Blob container name
    pub container: String,
    /// SAS token (preferred)
    pub sas_token: Option<String>,
    /// Shared Key (fallback)
    pub account_key: Option<String>,
    /// Custom endpoint (e.g., for Azurite)
    pub endpoint: Option<String>,
}

/// Handle to the sqlite-objs VFS.
///
/// The VFS is registered globally and persists for the lifetime of the process.
/// This is a zero-sized type that provides static methods for VFS registration.
pub struct SqliteObjsVfs;

impl SqliteObjsVfs {
    /// Register the sqlite-objs VFS using environment variables.
    ///
    /// Reads configuration from:
    /// - `AZURE_STORAGE_ACCOUNT`
    /// - `AZURE_STORAGE_CONTAINER`
    /// - `AZURE_STORAGE_SAS` (checked first)
    /// - `AZURE_STORAGE_KEY` (fallback)
    ///
    /// # Arguments
    ///
    /// * `make_default` - If true, sqlite-objs becomes the default VFS for all connections
    ///
    /// # Errors
    ///
    /// Returns an error if registration fails (e.g., missing environment variables).
    pub fn register(make_default: bool) -> Result<()> {
        let rc = unsafe { sqlite_objs_sys::sqlite_objs_vfs_register(make_default as i32) };
        if rc == sqlite_objs_sys::SQLITE_OK {
            Ok(())
        } else {
            Err(SqliteObjsError::RegistrationFailed(format!(
                "sqlite_objs_vfs_register returned {}",
                rc
            )))
        }
    }

    /// Register the sqlite-objs VFS with explicit configuration.
    ///
    /// # Arguments
    ///
    /// * `config` - Azure Storage configuration
    /// * `make_default` - If true, sqlite-objs becomes the default VFS for all connections
    ///
    /// # Errors
    ///
    /// Returns an error if the configuration contains invalid data (null bytes)
    /// or if registration fails.
    pub fn register_with_config(config: &SqliteObjsConfig, make_default: bool) -> Result<()> {
        // Convert Rust strings to C strings
        let account = CString::new(config.account.as_str())
            .map_err(|_| SqliteObjsError::InvalidConfig("account contains null byte".into()))?;
        let container = CString::new(config.container.as_str())
            .map_err(|_| SqliteObjsError::InvalidConfig("container contains null byte".into()))?;

        let sas_token = config
            .sas_token
            .as_ref()
            .map(|s| CString::new(s.as_str()))
            .transpose()
            .map_err(|_| SqliteObjsError::InvalidConfig("sas_token contains null byte".into()))?;

        let account_key = config
            .account_key
            .as_ref()
            .map(|s| CString::new(s.as_str()))
            .transpose()
            .map_err(|_| SqliteObjsError::InvalidConfig("account_key contains null byte".into()))?;

        let endpoint = config
            .endpoint
            .as_ref()
            .map(|s| CString::new(s.as_str()))
            .transpose()
            .map_err(|_| SqliteObjsError::InvalidConfig("endpoint contains null byte".into()))?;

        let c_config = sqlite_objs_sys::sqlite_objs_config_t {
            account: account.as_ptr(),
            container: container.as_ptr(),
            sas_token: sas_token
                .as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(ptr::null()),
            account_key: account_key
                .as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(ptr::null()),
            endpoint: endpoint
                .as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(ptr::null()),
            ops: ptr::null(),
            ops_ctx: ptr::null_mut(),
        };

        let rc = unsafe {
            sqlite_objs_sys::sqlite_objs_vfs_register_with_config(&c_config, make_default as i32)
        };

        if rc == sqlite_objs_sys::SQLITE_OK {
            Ok(())
        } else {
            Err(SqliteObjsError::RegistrationFailed(format!(
                "sqlite_objs_vfs_register_with_config returned {}",
                rc
            )))
        }
    }

    /// Register the sqlite-objs VFS in URI mode.
    ///
    /// In this mode, Azure credentials must be provided via URI parameters for each database:
    ///
    /// ```text
    /// file:mydb.db?azure_account=acct&azure_container=cont&azure_sas=token
    /// ```
    ///
    /// Supported URI parameters:
    /// - `azure_account` (required)
    /// - `azure_container`
    /// - `azure_sas`
    /// - `azure_key`
    /// - `azure_endpoint`
    ///
    /// # Arguments
    ///
    /// * `make_default` - If true, sqlite-objs becomes the default VFS for all connections
    ///
    /// # Errors
    ///
    /// Returns an error if registration fails.
    pub fn register_uri(make_default: bool) -> Result<()> {
        let rc = unsafe { sqlite_objs_sys::sqlite_objs_vfs_register_uri(make_default as i32) };
        if rc == sqlite_objs_sys::SQLITE_OK {
            Ok(())
        } else {
            Err(SqliteObjsError::RegistrationFailed(format!(
                "sqlite_objs_vfs_register_uri returned {}",
                rc
            )))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_register_uri() {
        // URI mode should succeed without config
        SqliteObjsVfs::register_uri(false).expect("URI registration should succeed");
    }

    #[test]
    fn test_config_with_sas() {
        let config = SqliteObjsConfig {
            account: "testaccount".to_string(),
            container: "testcontainer".to_string(),
            sas_token: Some("sv=2024-08-04&sig=test".to_string()),
            account_key: None,
            endpoint: None,
        };

        // This will fail since we don't have real Azure creds,
        // but it tests the FFI layer
        let _ = SqliteObjsVfs::register_with_config(&config, false);
    }

    #[test]
    fn test_invalid_config() {
        let config = SqliteObjsConfig {
            account: "test\0account".to_string(),
            container: "container".to_string(),
            sas_token: None,
            account_key: None,
            endpoint: None,
        };

        let result = SqliteObjsVfs::register_with_config(&config, false);
        assert!(matches!(result, Err(SqliteObjsError::InvalidConfig(_))));
    }
}
