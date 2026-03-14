//! Basic example showing how to use sqlite-objs with rusqlite.
//!
//! This example demonstrates VFS registration and URI builder usage.
//! The VFS integrates with rusqlite's standard APIs.
//!
//! NOTE: This example just demonstrates successful VFS registration.
//! To actually use Azure Blob Storage, you need:
//!
//! 1. Azurite running locally:
//!    ```sh
//!    azurite-blob --silent --location /tmp/azurite &
//!    ```
//!
//! 2. Set environment variables OR use URI mode with Azure credentials
//!
//! See the README for full usage examples with Azure credentials.

use sqlite_objs::UriBuilder;

fn main() {
    println!("sqlite-objs VFS Registration Example\n");

    // Example 1: Environment variable registration
    println!("1. Registration from environment variables:");
    println!("   Set AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS");
    println!("   SqliteObjsVfs::register(false)?;\n");

    // Example 2: Explicit config registration
    println!("2. Registration with explicit config:");
    println!("   let config = SqliteObjsConfig {{");
    println!("       account: \"myaccount\".to_string(),");
    println!("       container: \"databases\".to_string(),");
    println!("       sas_token: Some(\"sv=2024-08-04&...\".to_string()),");
    println!("       account_key: None,");
    println!("       endpoint: None,");
    println!("   }};");
    println!("   SqliteObjsVfs::register_with_config(&config, false)?;\n");

    // Example 3: URI mode with URI builder
    println!("3. URI mode with UriBuilder (recommended for per-database credentials):");
    println!("   SqliteObjsVfs::register_uri(false)?;");
    println!();
    
    // Demonstrate URI builder
    let uri = UriBuilder::new("mydb.db", "myaccount", "databases")
        .sas_token("sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123")
        .build();
    
    println!("   let uri = UriBuilder::new(\"mydb.db\", \"myaccount\", \"databases\")");
    println!("       .sas_token(\"sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123\")");
    println!("       .build();");
    println!();
    println!("   Generated URI (SAS token is URL-encoded):");
    println!("   {}\n", uri);

    println!("   Connection::open_with_flags_and_vfs(");
    println!("       &uri,");
    println!("       SQLITE_OPEN_READ_WRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,");
    println!("       \"sqlite-objs\"");
    println!("   )?;\n");

    // Example 4: URI mode with endpoint (Azurite)
    println!("4. URI mode with custom endpoint (e.g., Azurite):");
    let azurite_uri = UriBuilder::new("testdb.db", "devstoreaccount1", "testcontainer")
        .sas_token("sv=2024-08-04&st=2025-01-01T00:00:00Z&se=2026-01-01T00:00:00Z&sr=c&sp=rwdlac&sig=test")
        .endpoint("http://127.0.0.1:10000/devstoreaccount1")
        .build();
    
    println!("   let uri = UriBuilder::new(\"testdb.db\", \"devstoreaccount1\", \"testcontainer\")");
    println!("       .sas_token(\"sv=2024-08-04&st=2025-01-01T00:00:00Z&se=2026-01-01T00:00:00Z&sr=c&sp=rwdlac&sig=test\")");
    println!("       .endpoint(\"http://127.0.0.1:10000/devstoreaccount1\")");
    println!("       .build();");
    println!();
    println!("   Generated URI:");
    println!("   {}\n", azurite_uri);

    println!("Why use UriBuilder?");
    println!("- Automatic URL encoding of SAS tokens (contains &, =, % characters)");
    println!("- Prevents malformed URIs that would break SQLite connection");
    println!("- Type-safe, builder pattern for clarity");
    println!("- No external dependencies (minimal percent-encoding implementation)");
}
