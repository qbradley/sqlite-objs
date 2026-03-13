//! Basic example showing how to use sqlite-objs with rusqlite.
//!
//! This example demonstrates VFS registration and basic API usage.
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

use sqlite_objs::SqliteObjsVfs;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== sqlite-objs Basic Example ===\n");

    // Demonstrate VFS registration in URI mode
    println!("Registering sqlite-objs VFS in URI mode...");
    SqliteObjsVfs::register_uri(false)?;
    println!("✓ VFS registered successfully\n");

    println!("The sqlite-objs VFS is now available for use with rusqlite.");
    println!("To open a database with Azure Blob Storage:\n");
    
    println!("  use rusqlite::{{Connection, OpenFlags}};");
    println!("  ");
    println!("  let conn = Connection::open_with_flags_and_vfs(");
    println!("      \"file:mydb.db?azure_account=acct&azure_container=cont&azure_sas=token\",");
    println!("      OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE | OpenFlags::SQLITE_OPEN_URI,");
    println!("      \"sqlite-objs\"");
    println!("  )?;\n");

    println!("Or use environment variables:");
    println!("  export AZURE_STORAGE_ACCOUNT=myaccount");
    println!("  export AZURE_STORAGE_CONTAINER=databases");
    println!("  export AZURE_STORAGE_SAS='sv=2024-08-04&...'");
    println!("  ");
    println!("  SqliteObjsVfs::register(false)?;");
    println!("  let conn = Connection::open_with_flags_and_vfs(");
    println!("      \"mydb.db\",");
    println!("      OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,");
    println!("      \"sqlite-objs\"");
    println!("  )?;\n");

    println!("✓ Example complete");

    Ok(())
}
