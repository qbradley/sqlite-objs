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
//! use sqlite_objs::{SqliteObjsVfs, UriBuilder};
//! use rusqlite::Connection;
//!
//! // Register VFS in URI mode (no global config)
//! SqliteObjsVfs::register_uri(false)?;
//!
//! // Build URI with proper URL encoding
//! let uri = UriBuilder::new("mydb.db", "myaccount", "databases")
//!     .sas_token("sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123")
//!     .build();
//!
//! // Open database with Azure credentials in URI
//! let conn = Connection::open_with_flags_and_vfs(
//!     &uri,
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
use std::fmt::Write;
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

/// Builder for constructing sqlite-objs URIs with proper URL encoding.
///
/// SQLite URIs use query parameters to pass Azure credentials. SAS tokens contain
/// special characters (`&`, `=`, `%`) that must be percent-encoded to avoid breaking
/// the URI query string.
///
/// # Example
///
/// ```
/// use sqlite_objs::UriBuilder;
///
/// let uri = UriBuilder::new("mydb.db", "myaccount", "databases")
///     .sas_token("sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123")
///     .build();
///
/// // URI is properly encoded:
/// // file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv%3D2024-08-04%26ss%3Db...
/// ```
///
/// # Authentication
///
/// Use either `sas_token()` or `account_key()`, not both. If both are set, `sas_token`
/// takes precedence.
pub struct UriBuilder {
    database: String,
    account: String,
    container: String,
    sas_token: Option<String>,
    account_key: Option<String>,
    endpoint: Option<String>,
}

impl UriBuilder {
    /// Create a new URI builder with required parameters.
    ///
    /// # Arguments
    ///
    /// * `database` - Database filename (e.g., "mydb.db")
    /// * `account` - Azure Storage account name
    /// * `container` - Blob container name
    pub fn new(database: &str, account: &str, container: &str) -> Self {
        Self {
            database: database.to_string(),
            account: account.to_string(),
            container: container.to_string(),
            sas_token: None,
            account_key: None,
            endpoint: None,
        }
    }

    /// Set the SAS token for authentication (preferred).
    ///
    /// The token will be URL-encoded automatically. Do not encode it yourself.
    pub fn sas_token(mut self, token: &str) -> Self {
        self.sas_token = Some(token.to_string());
        self
    }

    /// Set the account key for Shared Key authentication (fallback).
    ///
    /// The key will be URL-encoded automatically.
    pub fn account_key(mut self, key: &str) -> Self {
        self.account_key = Some(key.to_string());
        self
    }

    /// Set a custom endpoint (e.g., for Azurite: "http://127.0.0.1:10000").
    pub fn endpoint(mut self, endpoint: &str) -> Self {
        self.endpoint = Some(endpoint.to_string());
        self
    }

    /// Build the URI string with proper URL encoding.
    ///
    /// Returns a SQLite URI in the format:
    /// `file:{database}?azure_account={account}&azure_container={container}&...`
    pub fn build(self) -> String {
        let mut uri = format!("file:{}?azure_account={}&azure_container={}", 
            self.database, 
            percent_encode(&self.account),
            percent_encode(&self.container)
        );

        // Prefer SAS token over account key
        if let Some(sas) = &self.sas_token {
            uri.push_str("&azure_sas=");
            uri.push_str(&percent_encode(sas));
        } else if let Some(key) = &self.account_key {
            uri.push_str("&azure_key=");
            uri.push_str(&percent_encode(key));
        }

        if let Some(endpoint) = &self.endpoint {
            uri.push_str("&azure_endpoint=");
            uri.push_str(&percent_encode(endpoint));
        }

        uri
    }
}

/// Percent-encode a string for use in URI query parameters.
///
/// Encodes characters that have special meaning in URIs:
/// - Reserved: `&`, `=`, `%`, `#`, `?`, `+`, `/`, `:`, `@`
/// - Space
///
/// This is a minimal implementation sufficient for SQLite URI parameters.
/// Uses uppercase hex digits per RFC 3986.
fn percent_encode(s: &str) -> String {
    let mut result = String::with_capacity(s.len() * 2);
    
    for byte in s.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                // Unreserved characters (RFC 3986 section 2.3)
                result.push(byte as char);
            }
            _ => {
                // Encode everything else
                write!(result, "%{:02X}", byte).unwrap();
            }
        }
    }
    
    result
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

    #[test]
    fn test_uri_builder_basic() {
        let uri = UriBuilder::new("mydb.db", "myaccount", "mycontainer").build();
        assert_eq!(uri, "file:mydb.db?azure_account=myaccount&azure_container=mycontainer");
    }

    #[test]
    fn test_uri_builder_with_sas() {
        let uri = UriBuilder::new("mydb.db", "myaccount", "mycontainer")
            .sas_token("sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123")
            .build();
        
        // Verify SAS token is encoded (&, =, :)
        assert!(uri.contains("azure_sas=sv%3D2024-08-04%26ss%3Db%26srt%3Dsco%26sp%3Drwdlacyx%26se%3D2026-01-01T00%3A00%3A00Z%26sig%3Dabc123"));
        assert!(uri.starts_with("file:mydb.db?azure_account=myaccount&azure_container=mycontainer&azure_sas="));
    }

    #[test]
    fn test_uri_builder_with_account_key() {
        let uri = UriBuilder::new("test.db", "account", "container")
            .account_key("my/secret+key==")
            .build();
        
        // Verify account key is encoded (/, +, =)
        assert!(uri.contains("azure_key=my%2Fsecret%2Bkey%3D%3D"));
    }

    #[test]
    fn test_uri_builder_with_endpoint() {
        let uri = UriBuilder::new("test.db", "devstoreaccount1", "testcontainer")
            .endpoint("http://127.0.0.1:10000/devstoreaccount1")
            .build();
        
        // Verify endpoint is encoded (://)
        assert!(uri.contains("azure_endpoint=http%3A%2F%2F127.0.0.1%3A10000%2Fdevstoreaccount1"));
    }

    #[test]
    fn test_uri_builder_sas_precedence() {
        let uri = UriBuilder::new("test.db", "account", "container")
            .sas_token("sas_token_value")
            .account_key("key_value")
            .build();
        
        // SAS token should be present, account key should not
        assert!(uri.contains("azure_sas="));
        assert!(!uri.contains("azure_key="));
    }

    #[test]
    fn test_percent_encode_special_chars() {
        // Test all special characters that need encoding
        assert_eq!(percent_encode("hello&world"), "hello%26world");
        assert_eq!(percent_encode("key=value"), "key%3Dvalue");
        assert_eq!(percent_encode("100%"), "100%25");
        assert_eq!(percent_encode("a#b"), "a%23b");
        assert_eq!(percent_encode("a?b"), "a%3Fb");
        assert_eq!(percent_encode("a+b"), "a%2Bb");
        assert_eq!(percent_encode("a/b"), "a%2Fb");
        assert_eq!(percent_encode("a:b"), "a%3Ab");
        assert_eq!(percent_encode("a@b"), "a%40b");
        assert_eq!(percent_encode("hello world"), "hello%20world");
    }

    #[test]
    fn test_percent_encode_unreserved() {
        // Unreserved characters should not be encoded
        assert_eq!(percent_encode("azAZ09-_.~"), "azAZ09-_.~");
    }

    #[test]
    fn test_percent_encode_empty() {
        assert_eq!(percent_encode(""), "");
    }
}
