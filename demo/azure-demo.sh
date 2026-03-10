#!/bin/bash
#
# azure-demo.sh — Simple demonstration of azqlite connecting to Azure Blob Storage
#
# PREREQUISITES:
#   1. Set environment variables with your Azure credentials:
#
#      export AZURE_STORAGE_ACCOUNT="youraccount"
#      export AZURE_STORAGE_CONTAINER="databases"
#
#      # Option A: Use a SAS token (recommended)
#      export AZURE_STORAGE_SAS="sv=2024-08-04&ss=b&srt=sco&sp=rwdlac&se=..."
#
#      # Option B: Use a Shared Key (fallback)
#      export AZURE_STORAGE_KEY="your-base64-encoded-account-key"
#
#   2. Run this script from the project root:
#      ./demo/azure-demo.sh
#
# This script will:
#   - Build azqlite-shell with production Azure client
#   - Open a database in Azure Blob Storage
#   - Create a table, insert data, query it
#   - Clean up
#

set -e  # Exit on error

# Color output for clarity
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== azqlite Azure Blob Storage Demo ===${NC}"
echo

# Check environment variables
echo -e "${YELLOW}Checking environment variables...${NC}"
if [[ -z "$AZURE_STORAGE_ACCOUNT" ]]; then
    echo -e "${RED}ERROR: AZURE_STORAGE_ACCOUNT is not set${NC}"
    echo "Set it to your Azure Storage account name:"
    echo "  export AZURE_STORAGE_ACCOUNT=\"youraccount\""
    exit 1
fi

if [[ -z "$AZURE_STORAGE_CONTAINER" ]]; then
    echo -e "${RED}ERROR: AZURE_STORAGE_CONTAINER is not set${NC}"
    echo "Set it to the blob container name:"
    echo "  export AZURE_STORAGE_CONTAINER=\"databases\""
    exit 1
fi

if [[ -z "$AZURE_STORAGE_SAS" && -z "$AZURE_STORAGE_KEY" ]]; then
    echo -e "${RED}ERROR: Neither AZURE_STORAGE_SAS nor AZURE_STORAGE_KEY is set${NC}"
    echo "Set one of them:"
    echo "  export AZURE_STORAGE_SAS=\"sv=2024-08-04&...\""
    echo "  export AZURE_STORAGE_KEY=\"your-base64-key\""
    exit 1
fi

echo -e "  ${GREEN}✓${NC} AZURE_STORAGE_ACCOUNT = $AZURE_STORAGE_ACCOUNT"
echo -e "  ${GREEN}✓${NC} AZURE_STORAGE_CONTAINER = $AZURE_STORAGE_CONTAINER"
if [[ -n "$AZURE_STORAGE_SAS" ]]; then
    echo -e "  ${GREEN}✓${NC} AZURE_STORAGE_SAS = [set, ${#AZURE_STORAGE_SAS} chars]"
else
    echo -e "  ${GREEN}✓${NC} AZURE_STORAGE_KEY = [set, ${#AZURE_STORAGE_KEY} chars]"
fi
echo

# Build the production shell
echo -e "${YELLOW}Building azqlite-shell with production Azure client...${NC}"
make clean > /dev/null 2>&1
if ! make all-production > /dev/null 2>&1; then
    echo -e "${RED}ERROR: Build failed. Check that libcurl and OpenSSL are installed.${NC}"
    echo "On macOS with Homebrew:"
    echo "  brew install curl openssl@3"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} Build succeeded"
echo

# Define a test database name
DB_NAME="demo-$(date +%s).db"

echo -e "${YELLOW}Running SQLite commands against Azure Blob: ${DB_NAME}${NC}"
echo

# Create a SQL script to run
SQL_SCRIPT=$(cat <<'EOF'
-- Create a simple table
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT UNIQUE
);

-- Insert some test data
INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com');
INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com');
INSERT INTO users (name, email) VALUES ('Charlie', 'charlie@example.com');

-- Query the data
SELECT 'Inserted ' || COUNT(*) || ' users' AS result FROM users;
SELECT '---' AS separator;
SELECT 'User list:' AS result;
SELECT '  ID=' || id || ', Name=' || name || ', Email=' || email AS result FROM users;

-- Show we can update
UPDATE users SET email = 'alice.updated@example.com' WHERE name = 'Alice';
SELECT '---' AS separator;
SELECT 'After update:' AS result;
SELECT '  Alice email: ' || email AS result FROM users WHERE name = 'Alice';
EOF
)

# Run the SQL commands through azqlite-shell
echo "$SQL_SCRIPT" | ./azqlite-shell "$DB_NAME" 2>&1 | grep -E "^(Inserted|User|---| |After)"

echo
echo -e "${GREEN}=== Demo completed successfully! ===${NC}"
echo
echo "The database '$DB_NAME' now exists as a page blob in your Azure container."
echo "You can verify this using Azure Storage Explorer or the Azure portal."
echo
echo "To clean up the blob:"
echo "  az storage blob delete \\"
echo "    --account-name $AZURE_STORAGE_ACCOUNT \\"
echo "    --container-name $AZURE_STORAGE_CONTAINER \\"
echo "    --name $DB_NAME"
echo
