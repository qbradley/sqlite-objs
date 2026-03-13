# Azure Demo

This directory contains a demonstration script showing sqlite-objs in action with real Azure Blob Storage.

## Prerequisites

1. **Azure Storage Account**: You need an Azure Storage account with a blob container.

2. **Authentication**: Set environment variables for Azure credentials. Choose one:

   **Option A: SAS Token (recommended)**
   ```bash
   export AZURE_STORAGE_ACCOUNT="youraccount"
   export AZURE_STORAGE_CONTAINER="databases"
   export AZURE_STORAGE_SAS="sv=2024-08-04&ss=b&srt=sco&sp=rwdlac&se=..."
   ```

   **Option B: Shared Key**
   ```bash
   export AZURE_STORAGE_ACCOUNT="youraccount"
   export AZURE_STORAGE_CONTAINER="databases"
   export AZURE_STORAGE_KEY="your-base64-encoded-account-key"
   ```

3. **Dependencies**: Ensure libcurl and OpenSSL are installed:
   ```bash
   # macOS (Homebrew)
   brew install curl openssl@3
   
   # Ubuntu/Debian
   sudo apt-get install libcurl4-openssl-dev libssl-dev
   
   # RHEL/CentOS
   sudo yum install libcurl-devel openssl-devel
   ```

## Running the Demo

From the project root directory:

```bash
./demo/azure-demo.sh
```

The script will:
1. Verify environment variables are set
2. Build `sqlite-objs-shell` with production Azure client
3. Create a database in Azure Blob Storage
4. Create a table and insert test data
5. Query and update the data
6. Show you the results

## What Gets Created

The demo creates a **page blob** in your Azure container named `demo-{timestamp}.db`. This is a real SQLite database stored entirely in Azure Blob Storage.

You can:
- View it in Azure Portal or Azure Storage Explorer
- Connect to it again using `sqlite-objs-shell demo-{timestamp}.db`
- Delete it when done (see cleanup command in script output)

## Troubleshooting

**Build fails with "openssl/hmac.h not found":**
- Ensure OpenSSL is installed and pkg-config is available
- On macOS: `brew install openssl@3 pkg-config`

**VFS registration fails:**
- Check that all three environment variables are set correctly
- Verify your SAS token or Shared Key is valid
- Ensure the container exists in your storage account

**Connection errors:**
- Verify your storage account name is correct
- Check that the container has proper permissions
- For SAS tokens, ensure they haven't expired and have the right permissions (read, write, delete, list, add, create)
