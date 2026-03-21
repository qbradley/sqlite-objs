//! Strongly-typed VFS activity metrics.
//!
//! The sqlite-objs VFS tracks per-connection I/O counters. These are
//! returned as a newline-separated `key=value` text string via
//! [`SQLITE_OBJS_FCNTL_STATS`](sqlite_objs_sys::SQLITE_OBJS_FCNTL_STATS)
//! or `PRAGMA sqlite_objs_stats`.
//!
//! This module provides [`VfsMetrics`] — a typed struct that parses the C
//! output into native Rust i64 fields — and a [`ParseError`] for malformed
//! input.
//!
//! # Example
//!
//! ```
//! use sqlite_objs::metrics::VfsMetrics;
//!
//! let text = "\
//!     disk_reads=10\n\
//!     disk_writes=5\n\
//!     disk_bytes_read=40960\n\
//!     disk_bytes_written=20480\n\
//!     blob_reads=2\n\
//!     blob_writes=1\n\
//!     blob_bytes_read=1048576\n\
//!     blob_bytes_written=4096\n\
//!     cache_hits=100\n\
//!     cache_misses=3\n\
//!     cache_miss_pages=12\n\
//!     prefetch_pages=256\n\
//!     lease_acquires=1\n\
//!     lease_renewals=0\n\
//!     lease_releases=1\n\
//!     syncs=2\n\
//!     dirty_pages_synced=5\n\
//!     blob_resizes=0\n\
//!     revalidations=1\n\
//!     revalidation_downloads=0\n\
//!     revalidation_diffs=1\n\
//!     pages_invalidated=0\n\
//!     journal_uploads=1\n\
//!     journal_bytes_uploaded=4096\n\
//!     wal_uploads=0\n\
//!     wal_bytes_uploaded=0\n\
//!     azure_errors=0";
//!
//! let m = VfsMetrics::parse(text).unwrap();
//! assert_eq!(m.disk_reads, 10);
//! assert_eq!(m.cache_hits, 100);
//! ```

use std::fmt;

/// Error returned when [`VfsMetrics::parse`] cannot interpret the C output.
#[derive(Debug, Clone)]
pub struct ParseError {
    /// Human-readable description of what went wrong.
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "metrics parse error: {}", self.message)
    }
}

impl std::error::Error for ParseError {}

/// Per-connection VFS activity counters.
///
/// All fields are signed 64-bit integers matching the C `sqlite_objs_metrics`
/// struct. Counters are zeroed when the database file is opened and can be
/// reset at any time via
/// [`SQLITE_OBJS_FCNTL_STATS_RESET`](sqlite_objs_sys::SQLITE_OBJS_FCNTL_STATS_RESET).
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct VfsMetrics {
    // -- Disk I/O (local cache file) --
    /// Number of pread calls to the local cache file.
    pub disk_reads: i64,
    /// Number of pwrite calls to the local cache file.
    pub disk_writes: i64,
    /// Total bytes read from the local cache file.
    pub disk_bytes_read: i64,
    /// Total bytes written to the local cache file.
    pub disk_bytes_written: i64,

    // -- Azure Blob I/O (network) --
    /// Number of Azure blob read operations.
    pub blob_reads: i64,
    /// Number of Azure blob write operations.
    pub blob_writes: i64,
    /// Total bytes downloaded from Azure.
    pub blob_bytes_read: i64,
    /// Total bytes uploaded to Azure.
    pub blob_bytes_written: i64,

    // -- Cache behaviour --
    /// Page reads satisfied from the local cache.
    pub cache_hits: i64,
    /// Page reads that required a network fetch.
    pub cache_misses: i64,
    /// Individual pages fetched due to cache misses.
    pub cache_miss_pages: i64,
    /// Pages loaded during prefetch (full-blob download at open).
    pub prefetch_pages: i64,

    // -- Lease / locking --
    /// Number of blob lease acquisitions.
    pub lease_acquires: i64,
    /// Number of blob lease renewals.
    pub lease_renewals: i64,
    /// Number of blob lease releases.
    pub lease_releases: i64,

    // -- Sync --
    /// Number of xSync calls.
    pub syncs: i64,
    /// Dirty pages flushed to Azure during sync.
    pub dirty_pages_synced: i64,
    /// Number of blob resize operations.
    pub blob_resizes: i64,

    // -- Revalidation --
    /// Number of ETag revalidation checks.
    pub revalidations: i64,
    /// Revalidations that required a full re-download.
    pub revalidation_downloads: i64,
    /// Revalidations that applied an incremental diff.
    pub revalidation_diffs: i64,
    /// Pages invalidated by revalidation.
    pub pages_invalidated: i64,

    // -- Journal & WAL uploads --
    /// Number of journal file uploads.
    pub journal_uploads: i64,
    /// Total bytes of journal data uploaded.
    pub journal_bytes_uploaded: i64,
    /// Number of WAL file uploads.
    pub wal_uploads: i64,
    /// Total bytes of WAL data uploaded.
    pub wal_bytes_uploaded: i64,

    // -- Errors --
    /// Azure HTTP errors (after retry exhaustion).
    pub azure_errors: i64,
}

impl VfsMetrics {
    /// The number of counters in the metrics struct.
    pub const FIELD_COUNT: usize = 27;

    /// Parse the `key=value\n` text returned by FCNTL 201 / `PRAGMA sqlite_objs_stats`.
    ///
    /// Unrecognised keys are silently ignored so that older Rust code can
    /// read metrics from a newer C library that added counters. Missing
    /// keys default to zero.
    ///
    /// # Errors
    ///
    /// Returns [`ParseError`] if a recognised key has a non-integer value.
    pub fn parse(text: &str) -> Result<Self, ParseError> {
        let mut m = VfsMetrics::default();

        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }

            let (key, value) = match line.split_once('=') {
                Some(pair) => pair,
                None => {
                    return Err(ParseError {
                        message: format!("expected key=value, got: {line}"),
                    });
                }
            };

            let v: i64 = value.parse().map_err(|_| ParseError {
                message: format!("invalid integer for key '{key}': {value}"),
            })?;

            match key {
                "disk_reads" => m.disk_reads = v,
                "disk_writes" => m.disk_writes = v,
                "disk_bytes_read" => m.disk_bytes_read = v,
                "disk_bytes_written" => m.disk_bytes_written = v,
                "blob_reads" => m.blob_reads = v,
                "blob_writes" => m.blob_writes = v,
                "blob_bytes_read" => m.blob_bytes_read = v,
                "blob_bytes_written" => m.blob_bytes_written = v,
                "cache_hits" => m.cache_hits = v,
                "cache_misses" => m.cache_misses = v,
                "cache_miss_pages" => m.cache_miss_pages = v,
                "prefetch_pages" => m.prefetch_pages = v,
                "lease_acquires" => m.lease_acquires = v,
                "lease_renewals" => m.lease_renewals = v,
                "lease_releases" => m.lease_releases = v,
                "syncs" => m.syncs = v,
                "dirty_pages_synced" => m.dirty_pages_synced = v,
                "blob_resizes" => m.blob_resizes = v,
                "revalidations" => m.revalidations = v,
                "revalidation_downloads" => m.revalidation_downloads = v,
                "revalidation_diffs" => m.revalidation_diffs = v,
                "pages_invalidated" => m.pages_invalidated = v,
                "journal_uploads" => m.journal_uploads = v,
                "journal_bytes_uploaded" => m.journal_bytes_uploaded = v,
                "wal_uploads" => m.wal_uploads = v,
                "wal_bytes_uploaded" => m.wal_bytes_uploaded = v,
                "azure_errors" => m.azure_errors = v,
                _ => { /* forward-compatible: ignore unknown keys */ }
            }
        }

        Ok(m)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Full valid output matching the C formatMetrics() layout.
    const FULL_SAMPLE: &str = "\
disk_reads=10\n\
disk_writes=5\n\
disk_bytes_read=40960\n\
disk_bytes_written=20480\n\
blob_reads=2\n\
blob_writes=1\n\
blob_bytes_read=1048576\n\
blob_bytes_written=4096\n\
cache_hits=100\n\
cache_misses=3\n\
cache_miss_pages=12\n\
prefetch_pages=256\n\
lease_acquires=1\n\
lease_renewals=0\n\
lease_releases=1\n\
syncs=2\n\
dirty_pages_synced=5\n\
blob_resizes=0\n\
revalidations=1\n\
revalidation_downloads=0\n\
revalidation_diffs=1\n\
pages_invalidated=0\n\
journal_uploads=1\n\
journal_bytes_uploaded=4096\n\
wal_uploads=0\n\
wal_bytes_uploaded=0\n\
azure_errors=0";

    #[test]
    fn parse_full_sample() {
        let m = VfsMetrics::parse(FULL_SAMPLE).unwrap();
        assert_eq!(m.disk_reads, 10);
        assert_eq!(m.disk_writes, 5);
        assert_eq!(m.disk_bytes_read, 40960);
        assert_eq!(m.disk_bytes_written, 20480);
        assert_eq!(m.blob_reads, 2);
        assert_eq!(m.blob_writes, 1);
        assert_eq!(m.blob_bytes_read, 1_048_576);
        assert_eq!(m.blob_bytes_written, 4096);
        assert_eq!(m.cache_hits, 100);
        assert_eq!(m.cache_misses, 3);
        assert_eq!(m.cache_miss_pages, 12);
        assert_eq!(m.prefetch_pages, 256);
        assert_eq!(m.lease_acquires, 1);
        assert_eq!(m.lease_renewals, 0);
        assert_eq!(m.lease_releases, 1);
        assert_eq!(m.syncs, 2);
        assert_eq!(m.dirty_pages_synced, 5);
        assert_eq!(m.blob_resizes, 0);
        assert_eq!(m.revalidations, 1);
        assert_eq!(m.revalidation_downloads, 0);
        assert_eq!(m.revalidation_diffs, 1);
        assert_eq!(m.pages_invalidated, 0);
        assert_eq!(m.journal_uploads, 1);
        assert_eq!(m.journal_bytes_uploaded, 4096);
        assert_eq!(m.wal_uploads, 0);
        assert_eq!(m.wal_bytes_uploaded, 0);
        assert_eq!(m.azure_errors, 0);
    }

    #[test]
    fn parse_empty_string() {
        let m = VfsMetrics::parse("").unwrap();
        assert_eq!(m, VfsMetrics::default());
    }

    #[test]
    fn parse_missing_keys_default_to_zero() {
        let text = "disk_reads=42\ncache_hits=7";
        let m = VfsMetrics::parse(text).unwrap();
        assert_eq!(m.disk_reads, 42);
        assert_eq!(m.cache_hits, 7);
        assert_eq!(m.blob_reads, 0); // not in input
    }

    #[test]
    fn parse_unknown_keys_ignored() {
        let text = "disk_reads=1\nfuture_counter=999";
        let m = VfsMetrics::parse(text).unwrap();
        assert_eq!(m.disk_reads, 1);
    }

    #[test]
    fn parse_bad_integer_is_error() {
        let text = "disk_reads=not_a_number";
        let err = VfsMetrics::parse(text).unwrap_err();
        assert!(err.message.contains("disk_reads"));
        assert!(err.message.contains("not_a_number"));
    }

    #[test]
    fn parse_missing_equals_is_error() {
        let text = "disk_reads 10";
        let err = VfsMetrics::parse(text).unwrap_err();
        assert!(err.message.contains("key=value"));
    }

    #[test]
    fn parse_blank_lines_ignored() {
        let text = "\n\ndisk_reads=5\n\n\ncache_hits=3\n\n";
        let m = VfsMetrics::parse(text).unwrap();
        assert_eq!(m.disk_reads, 5);
        assert_eq!(m.cache_hits, 3);
    }

    #[test]
    fn parse_whitespace_trimmed() {
        let text = "  disk_reads=5  \n  cache_hits=3  ";
        let m = VfsMetrics::parse(text).unwrap();
        assert_eq!(m.disk_reads, 5);
        assert_eq!(m.cache_hits, 3);
    }

    #[test]
    fn parse_negative_values() {
        // Counters should never be negative in practice, but the parser
        // should not reject them (they are valid i64).
        let text = "disk_reads=-1";
        let m = VfsMetrics::parse(text).unwrap();
        assert_eq!(m.disk_reads, -1);
    }

    #[test]
    fn default_is_all_zeros() {
        let m = VfsMetrics::default();
        assert_eq!(m.disk_reads, 0);
        assert_eq!(m.azure_errors, 0);
        assert_eq!(m.wal_bytes_uploaded, 0);
    }
}
