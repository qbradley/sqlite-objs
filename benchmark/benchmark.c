/*
** benchmark.c — Performance comparison harness for azqlite
**
** This harness runs SQLite's speedtest1 to compare local SQLite 
** performance against azqlite (Azure blob-backed) performance.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct BenchmarkResult {
  double elapsed_seconds;
  int success;
  char error_msg[512];
} BenchmarkResult;

typedef enum OutputFormat {
  FORMAT_TEXT,
  FORMAT_CSV
} OutputFormat;

static double get_time_seconds(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/*
** Run a command and capture timing
*/
static BenchmarkResult run_command(const char *cmd) {
  BenchmarkResult result = {0};
  double start_time, end_time;
  
  start_time = get_time_seconds();
  int rc = system(cmd);
  end_time = get_time_seconds();
  
  result.elapsed_seconds = end_time - start_time;
  result.success = (WIFEXITED(rc) && (WEXITSTATUS(rc) == 0 || WEXITSTATUS(rc) == 1));
  
  if (!result.success) {
    snprintf(result.error_msg, sizeof(result.error_msg), 
             "Command failed with status %d", rc);
  }
  
  return result;
}

/*
** Check if Azure environment variables are set
*/
static int check_azure_env(void) {
  const char *account = getenv("AZURE_STORAGE_ACCOUNT");
  const char *key = getenv("AZURE_STORAGE_KEY");
  const char *sas = getenv("AZURE_STORAGE_SAS");
  const char *container = getenv("AZURE_STORAGE_CONTAINER");
  
  if (!account || !container) {
    fprintf(stderr, "Error: AZURE_STORAGE_ACCOUNT and AZURE_STORAGE_CONTAINER must be set\n");
    return 0;
  }
  
  if (!key && !sas) {
    fprintf(stderr, "Error: Either AZURE_STORAGE_KEY or AZURE_STORAGE_SAS must be set\n");
    return 0;
  }
  
  return 1;
}

/*
** Print results in text format
*/
static void print_text_results(BenchmarkResult *local, BenchmarkResult *azure, int run_both) {
  printf("\n");
  printf("=================================================================\n");
  printf("                   BENCHMARK RESULTS\n");
  printf("=================================================================\n");
  printf("\n");
  
  if (local && local->success) {
    printf("Local SQLite (default VFS):\n");
    printf("  Elapsed time:  %.3f seconds\n", local->elapsed_seconds);
    printf("\n");
  } else if (local && !local->success) {
    printf("Local SQLite (default VFS):\n");
    printf("  FAILED: %s\n", local->error_msg);
    printf("\n");
  }
  
  if (azure && azure->success) {
    printf("Azure SQLite (azqlite VFS):\n");
    printf("  Elapsed time:  %.3f seconds\n", azure->elapsed_seconds);
    printf("\n");
  } else if (azure && !azure->success) {
    printf("Azure SQLite (azqlite VFS):\n");
    printf("  FAILED: %s\n", azure->error_msg);
    printf("\n");
  }
  
  if (run_both && local->success && azure->success) {
    double ratio = azure->elapsed_seconds / local->elapsed_seconds;
    printf("Performance Comparison:\n");
    printf("  Azure vs Local:  %.2fx", ratio);
    if (ratio > 1.0) {
      printf(" (%.1f%% slower)\n", (ratio - 1.0) * 100.0);
    } else {
      printf(" (%.1f%% faster)\n", (1.0 - ratio) * 100.0);
    }
    printf("\n");
  }
  
  printf("=================================================================\n");
}

/*
** Print results in CSV format
*/
static void print_csv_results(BenchmarkResult *local, BenchmarkResult *azure) {
  printf("test,elapsed_seconds,status\n");
  
  if (local) {
    printf("local,%.3f,%s\n", 
           local->elapsed_seconds, 
           local->success ? "success" : "failed");
  }
  
  if (azure) {
    printf("azure,%.3f,%s\n", 
           azure->elapsed_seconds, 
           azure->success ? "success" : "failed");
  }
}

/*
** Print usage information
*/
static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --local-only       Run only local SQLite benchmark\n");
  fprintf(stderr, "  --azure-only       Run only azqlite benchmark\n");
  fprintf(stderr, "  --size N           Size parameter for speedtest1 (default: 25)\n");
  fprintf(stderr, "  --output FORMAT    Output format: text (default) or csv\n");
  fprintf(stderr, "  --help             Show this help message\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Environment variables for Azure mode:\n");
  fprintf(stderr, "  AZURE_STORAGE_ACCOUNT   Storage account name\n");
  fprintf(stderr, "  AZURE_STORAGE_KEY       Shared key (or use SAS)\n");
  fprintf(stderr, "  AZURE_STORAGE_SAS       SAS token (or use KEY)\n");
  fprintf(stderr, "  AZURE_STORAGE_CONTAINER Container name\n");
}

int main(int argc, char **argv) {
  int run_local = 1;
  int run_azure = 1;
  int size_param = 25;
  OutputFormat format = FORMAT_TEXT;
  BenchmarkResult local_result = {0};
  BenchmarkResult azure_result = {0};
  char command[512];
  
  /* Parse command line arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--local-only") == 0) {
      run_local = 1;
      run_azure = 0;
    } else if (strcmp(argv[i], "--azure-only") == 0) {
      run_local = 0;
      run_azure = 1;
    } else if (strcmp(argv[i], "--size") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --size requires an argument\n");
        return 1;
      }
      size_param = atoi(argv[i]);
      if (size_param <= 0) {
        fprintf(stderr, "Error: --size must be positive\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--output") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --output requires an argument\n");
        return 1;
      }
      if (strcmp(argv[i], "text") == 0) {
        format = FORMAT_TEXT;
      } else if (strcmp(argv[i], "csv") == 0) {
        format = FORMAT_CSV;
      } else {
        fprintf(stderr, "Error: unknown output format '%s'\n", argv[i]);
        return 1;
      }
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }
  
  /* Check Azure environment if needed */
  if (run_azure && !check_azure_env()) {
    return 1;
  }
  
  /* Run local benchmark */
  if (run_local) {
    if (format == FORMAT_TEXT) {
      printf("\n");
      printf("Running local SQLite benchmark (size=%d)...\n", size_param);
      printf("-----------------------------------------------------------------\n");
    }
    
    /* Clean up any existing test database */
    unlink("benchmark_local.db");
    unlink("benchmark_local.db-journal");
    
    /* Build command */
    snprintf(command, sizeof(command), 
             "./speedtest1 --size %d benchmark_local.db >/dev/null 2>&1",
             size_param);
    
    local_result = run_command(command);
    
    /* Clean up */
    unlink("benchmark_local.db");
    unlink("benchmark_local.db-journal");
    
    if (format == FORMAT_TEXT) {
      printf("-----------------------------------------------------------------\n");
      if (local_result.success) {
        printf("Local benchmark completed in %.3f seconds\n", local_result.elapsed_seconds);
      } else {
        printf("Local benchmark FAILED: %s\n", local_result.error_msg);
      }
    }
  }
  
  /* Run Azure benchmark */
  if (run_azure) {
    if (format == FORMAT_TEXT) {
      printf("\n");
      printf("Running Azure SQLite benchmark (size=%d)...\n", size_param);
      printf("-----------------------------------------------------------------\n");
    }
    
    /* Build command for azqlite */
    snprintf(command, sizeof(command), 
             "./speedtest1-azure --size %d benchmark_azure.db >/dev/null 2>&1",
             size_param);
    
    azure_result = run_command(command);
    
    /* Clean up */
    unlink("benchmark_azure.db");
    
    if (format == FORMAT_TEXT) {
      printf("-----------------------------------------------------------------\n");
      if (azure_result.success) {
        printf("Azure benchmark completed in %.3f seconds\n", azure_result.elapsed_seconds);
      } else {
        printf("Azure benchmark FAILED: %s\n", azure_result.error_msg);
      }
    }
  }
  
  /* Print results */
  if (format == FORMAT_TEXT) {
    print_text_results(
      run_local ? &local_result : NULL,
      run_azure ? &azure_result : NULL,
      run_local && run_azure
    );
  } else {
    print_csv_results(
      run_local ? &local_result : NULL,
      run_azure ? &azure_result : NULL
    );
  }
  
  return 0;
}
