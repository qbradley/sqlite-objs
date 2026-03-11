/*
** tpcc_load.c - TPC-C data loader
**
** Populates the database with initial data according to TPC-C specification
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"
#include "tpcc_schema.h"

/* Random string generation */
static const char *alphanum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void random_string(char *buf, int min_len, int max_len) {
  int len = min_len + (rand() % (max_len - min_len + 1));
  for (int i = 0; i < len; i++) {
    buf[i] = alphanum[rand() % 62];
  }
  buf[len] = '\0';
}

static void random_zip(char *buf) {
  sprintf(buf, "%04d11111", rand() % 10000);
}

static void random_last_name(char *buf, int num) {
  static const char *syllables[] = {"BAR", "OUGHT", "ABLE", "PRI", "PRES", 
                                     "ESE", "ANTI", "CALLY", "ATION", "EING"};
  sprintf(buf, "%s%s%s", 
          syllables[(num / 100) % 10],
          syllables[(num / 10) % 10],
          syllables[num % 10]);
}

/* Load items table (100,000 items, constant across all warehouses) */
static int load_items(sqlite3 *db) {
  sqlite3_stmt *stmt = NULL;
  const char *sql = "INSERT INTO item VALUES (?, ?, ?, ?, ?)";
  int rc;
  
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Failed to prepare item insert: %s\n", sqlite3_errmsg(db));
    return rc;
  }
  
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  
  for (int i = 1; i <= TPCC_NUM_ITEMS; i++) {
    char name[25], data[51];
    random_string(name, 14, 24);
    random_string(data, 26, 50);
    
    sqlite3_bind_int(stmt, 1, i);
    sqlite3_bind_int(stmt, 2, rand() % 10000 + 1);
    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, (rand() % 9900 + 100) / 100.0);
    sqlite3_bind_text(stmt, 5, data, -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "Item insert failed: %s\n", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return rc;
    }
    
    sqlite3_reset(stmt);
    
    if (i % 10000 == 0) {
      printf("  Loaded %d items...\n", i);
    }
  }
  
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Item COMMIT failed: %s (rc=%d)\n", sqlite3_errmsg(db), rc);
    sqlite3_finalize(stmt);
    return rc;
  }
  sqlite3_finalize(stmt);
  
  printf("  Loaded %d items\n", TPCC_NUM_ITEMS);
  return SQLITE_OK;
}

/* Load data for a single warehouse */
static int load_warehouse(sqlite3 *db, int w_id) {
  sqlite3_stmt *stmt_wh = NULL, *stmt_dist = NULL, *stmt_cust = NULL;
  sqlite3_stmt *stmt_stock = NULL, *stmt_ord = NULL, *stmt_ol = NULL;
  sqlite3_stmt *stmt_no = NULL, *stmt_hist = NULL;
  char sql[512];
  int rc;
  
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  
  /* Insert warehouse */
  sprintf(sql, "INSERT INTO warehouse VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  sqlite3_prepare_v2(db, sql, -1, &stmt_wh, NULL);
  
  char name[11], street1[21], street2[21], city[21], state[3], zip[10];
  random_string(name, 6, 10);
  random_string(street1, 10, 20);
  random_string(street2, 10, 20);
  random_string(city, 10, 20);
  random_string(state, 2, 2);
  random_zip(zip);
  
  sqlite3_bind_int(stmt_wh, 1, w_id);
  sqlite3_bind_text(stmt_wh, 2, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_wh, 3, street1, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_wh, 4, street2, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_wh, 5, city, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_wh, 6, state, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_wh, 7, zip, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt_wh, 8, (rand() % 2000) / 10000.0);
  sqlite3_bind_double(stmt_wh, 9, 300000.00);
  
  sqlite3_step(stmt_wh);
  sqlite3_finalize(stmt_wh);
  
  /* Prepare statements for bulk inserts */
  sqlite3_prepare_v2(db, "INSERT INTO district VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt_dist, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO customer VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt_cust, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO stock VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt_stock, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO orders VALUES (?,?,?,?,?,?,?,?)", -1, &stmt_ord, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO order_line VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &stmt_ol, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO new_order VALUES (?,?,?)", -1, &stmt_no, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO history VALUES (?,?,?,?,?,?,?,?)", -1, &stmt_hist, NULL);
  
  /* Load stock for this warehouse */
  for (int i = 1; i <= TPCC_NUM_ITEMS; i++) {
    char dist[25], data[51];
    for (int d = 0; d < 10; d++) {
      random_string(dist, 24, 24);
    }
    random_string(data, 26, 50);
    
    sqlite3_bind_int(stmt_stock, 1, i);  /* s_i_id */
    sqlite3_bind_int(stmt_stock, 2, w_id); /* s_w_id */
    sqlite3_bind_int(stmt_stock, 3, rand() % 91 + 10); /* quantity 10-100 */
    
    /* 10 district info strings */
    for (int d = 4; d <= 13; d++) {
      random_string(dist, 24, 24);
      sqlite3_bind_text(stmt_stock, d, dist, -1, SQLITE_TRANSIENT);
    }
    
    sqlite3_bind_int(stmt_stock, 14, 0); /* s_ytd */
    sqlite3_bind_int(stmt_stock, 15, 0); /* s_order_cnt */
    sqlite3_bind_int(stmt_stock, 16, 0); /* s_remote_cnt */
    sqlite3_bind_text(stmt_stock, 17, data, -1, SQLITE_TRANSIENT);
    
    sqlite3_step(stmt_stock);
    sqlite3_reset(stmt_stock);
  }
  
  /* Load districts, customers, and orders */
  for (int d_id = 1; d_id <= TPCC_DISTRICTS_PER_WH; d_id++) {
    /* Insert district */
    random_string(name, 6, 10);
    random_string(street1, 10, 20);
    random_string(street2, 10, 20);
    random_string(city, 10, 20);
    random_string(state, 2, 2);
    random_zip(zip);
    
    sqlite3_bind_int(stmt_dist, 1, d_id);
    sqlite3_bind_int(stmt_dist, 2, w_id);
    sqlite3_bind_text(stmt_dist, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_dist, 4, street1, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_dist, 5, street2, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_dist, 6, city, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_dist, 7, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_dist, 8, zip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt_dist, 9, (rand() % 2000) / 10000.0);
    sqlite3_bind_double(stmt_dist, 10, 30000.00);
    sqlite3_bind_int(stmt_dist, 11, TPCC_INITIAL_ORDERS + 1);
    
    sqlite3_step(stmt_dist);
    sqlite3_reset(stmt_dist);
    
    /* Insert customers */
    for (int c_id = 1; c_id <= TPCC_CUSTOMERS_PER_DIST; c_id++) {
      char first[17], middle[3], last[17], phone[17], credit[3], data[501];
      
      random_string(first, 8, 16);
      strcpy(middle, "OE");
      random_last_name(last, c_id <= 1000 ? c_id - 1 : rand() % 1000);
      random_string(street1, 10, 20);
      random_string(street2, 10, 20);
      random_string(city, 10, 20);
      random_string(state, 2, 2);
      random_zip(zip);
      sprintf(phone, "%016d", rand());
      strcpy(credit, rand() % 10 == 0 ? "BC" : "GC");
      random_string(data, 300, 500);
      
      sqlite3_bind_int(stmt_cust, 1, c_id);
      sqlite3_bind_int(stmt_cust, 2, d_id);
      sqlite3_bind_int(stmt_cust, 3, w_id);
      sqlite3_bind_text(stmt_cust, 4, first, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 5, middle, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 6, last, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 7, street1, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 8, street2, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 9, city, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 10, state, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 11, zip, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 12, phone, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt_cust, 13, "2024-01-01 00:00:00", -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt_cust, 14, credit, -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(stmt_cust, 15, 50000.00);
      sqlite3_bind_double(stmt_cust, 16, (rand() % 5001) / 10000.0);
      sqlite3_bind_double(stmt_cust, 17, -10.00);
      sqlite3_bind_double(stmt_cust, 18, 10.00);
      sqlite3_bind_int(stmt_cust, 19, 1);
      sqlite3_bind_int(stmt_cust, 20, 0);
      sqlite3_bind_text(stmt_cust, 21, data, -1, SQLITE_TRANSIENT);
      
      sqlite3_step(stmt_cust);
      sqlite3_reset(stmt_cust);
      
      /* Insert history record */
      sqlite3_bind_int(stmt_hist, 1, c_id);
      sqlite3_bind_int(stmt_hist, 2, d_id);
      sqlite3_bind_int(stmt_hist, 3, w_id);
      sqlite3_bind_int(stmt_hist, 4, d_id);
      sqlite3_bind_int(stmt_hist, 5, w_id);
      sqlite3_bind_text(stmt_hist, 6, "2024-01-01 00:00:00", -1, SQLITE_STATIC);
      sqlite3_bind_double(stmt_hist, 7, 10.00);
      sqlite3_bind_text(stmt_hist, 8, "Initial", -1, SQLITE_STATIC);
      
      sqlite3_step(stmt_hist);
      sqlite3_reset(stmt_hist);
    }
    
    /* Insert initial orders */
    for (int o_id = 1; o_id <= TPCC_INITIAL_ORDERS; o_id++) {
      int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
      int ol_cnt = rand() % 11 + 5; /* 5-15 order lines */
      
      sqlite3_bind_int(stmt_ord, 1, o_id);
      sqlite3_bind_int(stmt_ord, 2, d_id);
      sqlite3_bind_int(stmt_ord, 3, w_id);
      sqlite3_bind_int(stmt_ord, 4, c_id);
      sqlite3_bind_text(stmt_ord, 5, "2024-01-01 00:00:00", -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt_ord, 6, o_id < 2101 ? rand() % 10 + 1 : 0); /* carrier */
      sqlite3_bind_int(stmt_ord, 7, ol_cnt);
      sqlite3_bind_int(stmt_ord, 8, 1);
      
      sqlite3_step(stmt_ord);
      sqlite3_reset(stmt_ord);
      
      /* New orders (last 900 orders) */
      if (o_id >= 2101) {
        sqlite3_bind_int(stmt_no, 1, o_id);
        sqlite3_bind_int(stmt_no, 2, d_id);
        sqlite3_bind_int(stmt_no, 3, w_id);
        
        sqlite3_step(stmt_no);
        sqlite3_reset(stmt_no);
      }
      
      /* Order lines */
      for (int ol_num = 1; ol_num <= ol_cnt; ol_num++) {
        int i_id = rand() % TPCC_NUM_ITEMS + 1;
        char dist_info[25];
        random_string(dist_info, 24, 24);
        
        sqlite3_bind_int(stmt_ol, 1, o_id);
        sqlite3_bind_int(stmt_ol, 2, d_id);
        sqlite3_bind_int(stmt_ol, 3, w_id);
        sqlite3_bind_int(stmt_ol, 4, ol_num);
        sqlite3_bind_int(stmt_ol, 5, i_id);
        sqlite3_bind_int(stmt_ol, 6, w_id);
        sqlite3_bind_text(stmt_ol, 7, o_id < 2101 ? "2024-01-01 00:00:00" : "", -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt_ol, 8, 5);
        sqlite3_bind_double(stmt_ol, 9, o_id < 2101 ? 0.00 : (rand() % 999999 + 1) / 100.0);
        sqlite3_bind_text(stmt_ol, 10, dist_info, -1, SQLITE_TRANSIENT);
        
        sqlite3_step(stmt_ol);
        sqlite3_reset(stmt_ol);
      }
    }
  }
  
  sqlite3_finalize(stmt_dist);
  sqlite3_finalize(stmt_cust);
  sqlite3_finalize(stmt_stock);
  sqlite3_finalize(stmt_ord);
  sqlite3_finalize(stmt_ol);
  sqlite3_finalize(stmt_no);
  sqlite3_finalize(stmt_hist);
  
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Warehouse COMMIT failed: %s (rc=%d)\n", sqlite3_errmsg(db), rc);
    return rc;
  }
  
  return SQLITE_OK;
}

/* Main loader entry point */
int tpcc_load_data(sqlite3 *db, int num_warehouses) {
  int rc;
  
  printf("\nLoading TPC-C data (warehouses=%d)...\n", num_warehouses);
  printf("=================================================================\n");
  
  /* Load items (shared across all warehouses) */
  printf("Loading items...\n");
  rc = load_items(db);
  if (rc != SQLITE_OK) {
    return rc;
  }
  
  /* Load warehouse-specific data */
  for (int w = 1; w <= num_warehouses; w++) {
    printf("Loading warehouse %d/%d...\n", w, num_warehouses);
    rc = load_warehouse(db, w);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to load warehouse %d\n", w);
      return rc;
    }
  }
  
  printf("=================================================================\n");
  printf("Data loading complete!\n");
  printf("  Items:     %d\n", TPCC_NUM_ITEMS);
  printf("  Warehouses: %d\n", num_warehouses);
  printf("  Districts:  %d\n", num_warehouses * TPCC_DISTRICTS_PER_WH);
  printf("  Customers:  %d\n", num_warehouses * TPCC_DISTRICTS_PER_WH * TPCC_CUSTOMERS_PER_DIST);
  printf("\n");
  
  return SQLITE_OK;
}
