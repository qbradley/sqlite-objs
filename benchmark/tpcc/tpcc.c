/*
** tpcc.c - TPC-C OLTP Benchmark for SQLite
**
** Measures OLTP performance against local SQLite vs azqlite (Azure blob-backed)
**
** Usage:
**   ./tpcc --local --warehouses 1 --duration 60
**   ./tpcc --azure --warehouses 2 --duration 120
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "sqlite3.h"
#include "azqlite.h"
#include "tpcc_schema.h"

/* Forward declarations from other TPC-C modules */
extern int tpcc_load_data(sqlite3 *db, int num_warehouses);

typedef struct {
  int success;
  char error_msg[256];
} txn_result_t;

extern txn_result_t tpcc_new_order_txn(sqlite3 *db, int w_id, int num_warehouses);
extern txn_result_t tpcc_payment_txn(sqlite3 *db, int w_id, int num_warehouses);
extern txn_result_t tpcc_order_status_txn(sqlite3 *db, int w_id, int num_warehouses);
extern int  tpcc_prepare_stmts(sqlite3 *db);
extern void tpcc_finalize_stmts(void);

/* Transaction statistics */
typedef struct {
  int count;
  int failures;
  double total_latency_ms;
  double min_latency_ms;
  double max_latency_ms;
  double *latencies; /* Array for percentile calculation */
  int latency_capacity;
} txn_stats_t;

/* Benchmark configuration */
typedef struct {
  int use_azure;
  int use_uri;
  int use_wal;
  int num_warehouses;
  int duration_seconds;
  int num_threads;
  int force_reload;
  int skip_load;
  char *db_path;
  /* URI mode fields */
  char *uri_account;
  char *uri_container;
  char *uri_sas;
  char *uri_endpoint;
  char *prefetch;               /* prefetch strategy: off, all, index, warm, or page count */
  char uri_db_path[4096];  /* constructed URI string (large for encoded SAS) */
} benchmark_config_t;

/*
** Percent-encode a URI query parameter value.
** SAS tokens contain '&' and '=' which break SQLite URI parameter parsing.
** Returns malloc'd string that caller must free, or NULL on OOM.
*/
static char *uri_encode_query_value(const char *input) {
  static const char hex[] = "0123456789ABCDEF";
  if (!input) return NULL;
  size_t len = strlen(input);
  /* Worst case: every char needs encoding (3x expansion) */
  char *output = malloc(len * 3 + 1);
  if (!output) return NULL;
  size_t j = 0;
  for (size_t i = 0; input[i]; i++) {
    unsigned char c = (unsigned char)input[i];
    /* RFC 3986 unreserved characters — safe in query values */
    int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved) {
      output[j++] = (char)c;
    } else {
      output[j++] = '%';
      output[j++] = hex[c >> 4];
      output[j++] = hex[c & 0x0F];
    }
  }
  output[j] = '\0';
  return output;
}

/* Initialize transaction stats */
static void init_stats(txn_stats_t *stats) {
  memset(stats, 0, sizeof(txn_stats_t));
  stats->min_latency_ms = 999999.0;
  stats->latency_capacity = 100000;
  stats->latencies = malloc(sizeof(double) * stats->latency_capacity);
}

/* Record a transaction latency */
static void record_latency(txn_stats_t *stats, double latency_ms, int success) {
  stats->count++;
  if (!success) {
    stats->failures++;
    return;
  }
  
  stats->total_latency_ms += latency_ms;
  if (latency_ms < stats->min_latency_ms) stats->min_latency_ms = latency_ms;
  if (latency_ms > stats->max_latency_ms) stats->max_latency_ms = latency_ms;
  
  /* Store for percentile calculation */
  if (stats->count - stats->failures - 1 < stats->latency_capacity) {
    stats->latencies[stats->count - stats->failures - 1] = latency_ms;
  }
}

/* Comparison function for qsort */
static int compare_double(const void *a, const void *b) {
  double diff = *(double*)a - *(double*)b;
  return (diff > 0) - (diff < 0);
}

/* Get percentile from latency array */
static double get_percentile(txn_stats_t *stats, double percentile) {
  if (stats->count - stats->failures <= 0) return 0.0;
  
  int n = stats->count - stats->failures;
  if (n > stats->latency_capacity) n = stats->latency_capacity;
  
  qsort(stats->latencies, n, sizeof(double), compare_double);
  
  int idx = (int)(n * percentile / 100.0);
  if (idx >= n) idx = n - 1;
  
  return stats->latencies[idx];
}

/* Get current time in milliseconds */
static double get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Print usage */
static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --local            Use local SQLite (default VFS)\n");
  fprintf(stderr, "  --azure            Use azqlite (Azure blob-backed VFS)\n");
  fprintf(stderr, "  --uri              Use URI-based Azure config (no env vars needed)\n");
  fprintf(stderr, "  --account NAME     Azure storage account (with --uri)\n");
  fprintf(stderr, "  --container NAME   Azure container name (with --uri)\n");
  fprintf(stderr, "  --sas TOKEN        SAS token (with --uri)\n");
  fprintf(stderr, "  --endpoint URL     Custom endpoint, e.g. Azurite (with --uri)\n");
  fprintf(stderr, "  --wal              Enable WAL journal mode (default)\n");
  fprintf(stderr, "  --warehouses N     Number of warehouses (default: 1)\n");
  fprintf(stderr, "  --duration S       Benchmark duration in seconds (default: 60)\n");
  fprintf(stderr, "  --threads N        Number of concurrent threads (default: 1)\n");
  fprintf(stderr, "  --reload           Force reload data (drops existing tables)\n");
  fprintf(stderr, "  --skip-load        Skip data loading (use existing data)\n");
  fprintf(stderr, "  --prefetch MODE    Prefetch strategy: off, all, index, warm, or page count\n");
  fprintf(stderr, "  --help             Show this help message\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "For Azure mode (env vars), set:\n");
  fprintf(stderr, "  AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER\n");
  fprintf(stderr, "  AZURE_STORAGE_KEY (or AZURE_STORAGE_SAS)\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "URI mode example:\n");
  fprintf(stderr, "  %s --uri --account myacct --container mycontainer --sas 'sv=2024...'\n", prog);
}

/* Check Azure environment */
static int check_azure_env(void) {
  const char *account = getenv("AZURE_STORAGE_ACCOUNT");
  const char *container = getenv("AZURE_STORAGE_CONTAINER");
  const char *key = getenv("AZURE_STORAGE_KEY");
  const char *sas = getenv("AZURE_STORAGE_SAS");
  
  if (!account || !container) {
    fprintf(stderr, "Error: AZURE_STORAGE_ACCOUNT and AZURE_STORAGE_CONTAINER required\n");
    return 0;
  }
  
  if (!key && !sas) {
    fprintf(stderr, "Error: AZURE_STORAGE_KEY or AZURE_STORAGE_SAS required\n");
    return 0;
  }
  
  return 1;
}

/* Run benchmark */
static int run_benchmark(benchmark_config_t *config) {
  sqlite3 *db = NULL;
  int rc;
  txn_stats_t neworder_stats, payment_stats, orderstatus_stats;
  double start_time, end_time, elapsed;
  int total_txns = 0;
  
  init_stats(&neworder_stats);
  init_stats(&payment_stats);
  init_stats(&orderstatus_stats);
  
  printf("\nTPC-C Benchmark Configuration\n");
  printf("=================================================================\n");
  printf("Mode:       %s\n", config->use_uri ? "azure (URI-mode VFS)" : config->use_azure ? "azure (azqlite VFS)" : "local (default VFS)");
  if (config->use_wal) {
    printf("Journal:    WAL (write-ahead logging)\n");
  } else {
    printf("Journal:    DELETE (rollback journal)\n");
  }
  printf("Warehouses: %d\n", config->num_warehouses);
  printf("Duration:   %d seconds\n", config->duration_seconds);
  printf("Threads:    %d\n", config->num_threads);
  printf("Database:   %s\n", config->db_path);
  if (config->prefetch) {
    printf("Prefetch:   %s\n", config->prefetch);
  }
  printf("=================================================================\n");
  
  /* Open database */
  printf("Opening database...\n");
  if (config->use_uri) {
#ifdef AZQLITE_VFS_AVAILABLE
    /* URI mode: register VFS with no global client */
    rc = azqlite_vfs_register_uri(1);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to register azqlite URI-mode VFS\n");
      return 1;
    }
    
    rc = sqlite3_open_v2(config->db_path, &db, 
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
                         "azqlite");
#else
    fprintf(stderr, "Error: URI mode requested but binary not built with Azure support\n");
    fprintf(stderr, "Build with 'make all-production' to enable Azure mode\n");
    return 1;
#endif
  } else if (config->use_azure) {
#ifdef AZQLITE_VFS_AVAILABLE
    /* Register azqlite VFS */

    rc = azqlite_vfs_register(1);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to register azqlite VFS\n");
      return 1;
    }
    
    rc = sqlite3_open_v2(config->db_path, &db, 
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         "azqlite");
#else
    fprintf(stderr, "Error: Azure mode requested but binary not built with Azure support\n");
    fprintf(stderr, "Build with 'make all-production' to enable Azure mode\n");
    return 1;
#endif
  } else {
    rc = sqlite3_open(config->db_path, &db);
  }
  
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  
  /* Set journal mode */
  printf("Set journal mode...\n");
  if (config->use_wal) {
    char *errmsg = NULL;

    rc = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to set locking_mode=EXCLUSIVE: %s\n",
              errmsg ? errmsg : sqlite3_errmsg(db));
      sqlite3_free(errmsg);
      sqlite3_close(db);
      return 1;
    }

    /* Switch to WAL journal mode and verify */
    sqlite3_stmt *jstmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &jstmt, NULL);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to prepare journal_mode pragma: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return 1;
    }
    const char *jmode = NULL;
    if (sqlite3_step(jstmt) == SQLITE_ROW) {
      jmode = (const char *)sqlite3_column_text(jstmt, 0);
    }
    if (!jmode || strcmp(jmode, "wal") != 0) {
      fprintf(stderr, "Failed to enable WAL mode (journal_mode is '%s', expected 'wal')\n",
              jmode ? jmode : "(null)");
      sqlite3_finalize(jstmt);
      sqlite3_close(db);
      return 1;
    }
    sqlite3_finalize(jstmt);
    printf("WAL mode enabled\n");
  } else {
    rc = sqlite3_exec(db, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Warning: failed to set journal_mode=DELETE: %s\n", sqlite3_errmsg(db));
    }
  }

  /* Set pragmas for performance */
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA cache_size=10000", NULL, NULL, NULL);
  
  /* Check if database has data (not just tables) */
  sqlite3_stmt *stmt = NULL;
  int needs_load = 0;
  
  if (config->skip_load) {
    printf("\nSkipping data load (--skip-load)\n");
    needs_load = 0;
  } else {
    /* Check if tables exist */
    printf("Check if tables exist...\n");
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'", -1, &stmt, NULL);
    int table_count = 0;
    if (rc == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        table_count = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
    }
    
    if (table_count == 0) {
      needs_load = 1;
    } else if (!config->force_reload) {
      /* Tables exist - check if data was loaded */
      printf("Table exist - check if data was loaded..");
      rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM district", -1, &stmt, NULL);
      int district_count = 0;
      if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          district_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
      }
      if (district_count == 0) {
        printf("\nWarning: Tables exist but no data found. Use --reload to force reload.\n");
        needs_load = 1;
      }
    }
    
    if (config->force_reload) {
      printf("\nDropping existing tables...\n");
      sqlite3_exec(db, "DROP TABLE IF EXISTS warehouse", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS district", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS customer", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS history", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS orders", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS new_order", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS order_line", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS item", NULL, NULL, NULL);
      sqlite3_exec(db, "DROP TABLE IF EXISTS stock", NULL, NULL, NULL);
      needs_load = 1;
    }
  }
  
  if (needs_load) {
    printf("\nCreating schema...\n");
    rc = sqlite3_exec(db, TPCC_CREATE_TABLES, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to create tables: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return 1;
    }
    
    printf("Loading data...\n");
    rc = tpcc_load_data(db, config->num_warehouses);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to load data\n");
      sqlite3_close(db);
      return 1;
    }
    
    /* Verify data was loaded successfully */
    sqlite3_stmt *vstmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM district", -1, &vstmt, NULL);
    int verify_count = 0;
    if (sqlite3_step(vstmt) == SQLITE_ROW) {
      verify_count = sqlite3_column_int(vstmt, 0);
    }
    sqlite3_finalize(vstmt);
    if (verify_count == 0) {
      fprintf(stderr, "ERROR: Data load reported success but no districts found!\n");
      sqlite3_close(db);
      return 1;
    }
    printf("\nVerified: %d districts loaded successfully.\n", verify_count);
  } else {
    printf("\nUsing existing database (skipping data load)\n");
  }
  
  printf("\nStarting benchmark run...\n");;
  printf("=================================================================\n");
  
  printf("Preparing statements...\n");
  if (tpcc_prepare_stmts(db) != 0) {
    fprintf(stderr, "Failed to prepare cached statements\n");
    sqlite3_close(db);
    return 1;
  }

  srand(time(NULL));
  start_time = get_time_ms();
  end_time = start_time + (config->duration_seconds * 1000.0);
  
  /* Main benchmark loop */
  printf("Main benchmark loop...\n");
  while (get_time_ms() < end_time) {
    int txn_type = rand() % 100;
    int w_id = rand() % config->num_warehouses + 1;
    double txn_start, txn_end, latency;
    txn_result_t result;
    
    txn_start = get_time_ms();
    
    if (txn_type < TPCC_MIX_NEWORDER) {
      /* New Order (45%) */
      result = tpcc_new_order_txn(db, w_id, config->num_warehouses);
      txn_end = get_time_ms();
      latency = txn_end - txn_start;
      record_latency(&neworder_stats, latency, result.success);
      
      if (!result.success && neworder_stats.failures <= 5) {
        fprintf(stderr, "New Order failed: %s\n", result.error_msg);
      }
    } else if (txn_type < TPCC_MIX_NEWORDER + TPCC_MIX_PAYMENT) {
      /* Payment (43%) */
      result = tpcc_payment_txn(db, w_id, config->num_warehouses);
      txn_end = get_time_ms();
      latency = txn_end - txn_start;
      record_latency(&payment_stats, latency, result.success);
      
      if (!result.success && payment_stats.failures <= 5) {
        fprintf(stderr, "Payment failed: %s\n", result.error_msg);
      }
    } else {
      /* Order Status (4%) - rest is simplified to just these 3 */
      result = tpcc_order_status_txn(db, w_id, config->num_warehouses);
      txn_end = get_time_ms();
      latency = txn_end - txn_start;
      record_latency(&orderstatus_stats, latency, result.success);
      
      if (!result.success && orderstatus_stats.failures <= 5) {
        fprintf(stderr, "Order Status failed: %s\n", result.error_msg);
      }
    }
    
    total_txns++;
    
    /* Progress indicator every 100 transactions */
    if (total_txns % 100 == 0) {
      printf("  %d transactions...\r", total_txns);
      fflush(stdout);
    }
  }
  
  elapsed = (get_time_ms() - start_time) / 1000.0;
  
  printf("\n=================================================================\n");
  printf("\nTPC-C Benchmark Results\n");
  printf("=================================================================\n");
  printf("Mode:       %s\n", config->use_uri ? "azure (URI-mode VFS)" : config->use_azure ? "azure (azqlite VFS)" : "local (default VFS)");
  printf("Journal:    %s\n", config->use_wal ? "WAL" : "DELETE");
  printf("Warehouses: %d\n", config->num_warehouses);
  printf("Duration:   %.1f seconds\n", elapsed);
  printf("Threads:    %d\n", config->num_threads);
  printf("\n");
  
  int new_success = neworder_stats.count - neworder_stats.failures;
  int pay_success = payment_stats.count - payment_stats.failures;
  int ord_success = orderstatus_stats.count - orderstatus_stats.failures;
  int total_success = new_success + pay_success + ord_success;
  
  printf("Transaction Mix:\n");
  
  if (neworder_stats.count > 0) {
    printf("  New Order:    %5d (%4.1f%%)  avg: %6.1fms  p50: %6.1fms  p95: %6.1fms  p99: %6.1fms",
           new_success,
           (new_success * 100.0) / total_success,
           neworder_stats.total_latency_ms / new_success,
           get_percentile(&neworder_stats, 50),
           get_percentile(&neworder_stats, 95),
           get_percentile(&neworder_stats, 99));
    if (neworder_stats.failures > 0) {
      printf("  (%d failed)", neworder_stats.failures);
    }
    printf("\n");
  }
  
  if (payment_stats.count > 0) {
    printf("  Payment:      %5d (%4.1f%%)  avg: %6.1fms  p50: %6.1fms  p95: %6.1fms  p99: %6.1fms",
           pay_success,
           (pay_success * 100.0) / total_success,
           payment_stats.total_latency_ms / pay_success,
           get_percentile(&payment_stats, 50),
           get_percentile(&payment_stats, 95),
           get_percentile(&payment_stats, 99));
    if (payment_stats.failures > 0) {
      printf("  (%d failed)", payment_stats.failures);
    }
    printf("\n");
  }
  
  if (orderstatus_stats.count > 0) {
    printf("  Order Status: %5d (%4.1f%%)  avg: %6.1fms  p50: %6.1fms  p95: %6.1fms  p99: %6.1fms",
           ord_success,
           (ord_success * 100.0) / total_success,
           orderstatus_stats.total_latency_ms / ord_success,
           get_percentile(&orderstatus_stats, 50),
           get_percentile(&orderstatus_stats, 95),
           get_percentile(&orderstatus_stats, 99));
    if (orderstatus_stats.failures > 0) {
      printf("  (%d failed)", orderstatus_stats.failures);
    }
    printf("\n");
  }
  
  printf("\n");
  printf("Total:      %5d transactions\n", total_success);
  printf("Throughput: %.1f tps\n", total_success / elapsed);
  printf("=================================================================\n");
  
  /* Cleanup */
  tpcc_finalize_stmts();
  free(neworder_stats.latencies);
  free(payment_stats.latencies);
  free(orderstatus_stats.latencies);
  
  sqlite3_close(db);
  
  return 0;
}

int main(int argc, char **argv) {
  benchmark_config_t config = {0};
  
  /* Defaults */
  config.num_warehouses = 1;
  config.duration_seconds = 60;
  config.num_threads = 1;
  config.use_wal = 1;  /* WAL mode by default */
  config.db_path = NULL;
  
  /* Parse arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--local") == 0) {
      config.use_azure = 0;
    } else if (strcmp(argv[i], "--azure") == 0) {
      config.use_azure = 1;
    } else if (strcmp(argv[i], "--uri") == 0) {
      config.use_uri = 1;
      config.use_azure = 1;
    } else if (strcmp(argv[i], "--account") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --account requires an argument\n");
        return 1;
      }
      config.uri_account = argv[i];
    } else if (strcmp(argv[i], "--container") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --container requires an argument\n");
        return 1;
      }
      config.uri_container = argv[i];
    } else if (strcmp(argv[i], "--sas") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --sas requires an argument\n");
        return 1;
      }
      config.uri_sas = argv[i];
    } else if (strcmp(argv[i], "--endpoint") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --endpoint requires an argument\n");
        return 1;
      }
      config.uri_endpoint = argv[i];
    } else if (strcmp(argv[i], "--warehouses") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --warehouses requires an argument\n");
        return 1;
      }
      config.num_warehouses = atoi(argv[i]);
      if (config.num_warehouses <= 0) {
        fprintf(stderr, "Error: --warehouses must be positive\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--duration") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --duration requires an argument\n");
        return 1;
      }
      config.duration_seconds = atoi(argv[i]);
      if (config.duration_seconds <= 0) {
        fprintf(stderr, "Error: --duration must be positive\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--threads") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --threads requires an argument\n");
        return 1;
      }
      config.num_threads = atoi(argv[i]);
      if (config.num_threads <= 0) {
        fprintf(stderr, "Error: --threads must be positive\n");
        return 1;
      }
      if (config.num_threads > 1) {
        fprintf(stderr, "Warning: multi-threading not yet implemented, using 1 thread\n");
        config.num_threads = 1;
      }
    } else if (strcmp(argv[i], "--reload") == 0) {
      config.force_reload = 1;
    } else if (strcmp(argv[i], "--wal") == 0) {
      config.use_wal = 1;
    } else if (strcmp(argv[i], "--skip-load") == 0) {
      config.skip_load = 1;
    } else if (strcmp(argv[i], "--prefetch") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --prefetch requires an argument (off, all, index, warm, or page count)\n");
        return 1;
      }
      config.prefetch = argv[i];
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }
  
  /* WAL mode is now the default and works with both local and Azure */

  /* Set database path */
  if (config.use_uri) {
    /* Validate URI mode args */
    if (!config.uri_account) {
      fprintf(stderr, "Error: --uri requires --account\n");
      return 1;
    }
    if (!config.uri_container) {
      fprintf(stderr, "Error: --uri requires --container\n");
      return 1;
    }
    if (!config.uri_sas) {
      fprintf(stderr, "Error: --uri requires --sas\n");
      return 1;
    }
    /* Percent-encode URI parameter values — SAS tokens contain '&' and '='
    ** which would be misinterpreted as URI parameter delimiters by SQLite */
    char *enc_sas = uri_encode_query_value(config.uri_sas);
    if (!enc_sas) {
      fprintf(stderr, "Error: failed to encode SAS token\n");
      return 1;
    }
    int n = snprintf(config.uri_db_path, sizeof(config.uri_db_path),
                     "file:tpcc.db?azure_account=%s&azure_container=%s&azure_sas=%s",
                     config.uri_account, config.uri_container, enc_sas);
    free(enc_sas);
    if (config.uri_endpoint) {
      char *enc_endpoint = uri_encode_query_value(config.uri_endpoint);
      if (!enc_endpoint) {
        fprintf(stderr, "Error: failed to encode endpoint\n");
        return 1;
      }
      n += snprintf(config.uri_db_path + n, sizeof(config.uri_db_path) - (size_t)n,
               "&azure_endpoint=%s", enc_endpoint);
      free(enc_endpoint);
    }
    if (config.prefetch) {
      snprintf(config.uri_db_path + n, sizeof(config.uri_db_path) - (size_t)n,
               "&prefetch=%s", config.prefetch);
    }
    config.db_path = config.uri_db_path;
  } else if (config.use_azure) {
    if (!check_azure_env()) {
      return 1;
    }
    config.db_path = "tpcc.db";
  } else {
    config.db_path = "tpcc_local.db";
  }
  
  return run_benchmark(&config);
}
