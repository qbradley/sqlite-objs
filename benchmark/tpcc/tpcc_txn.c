/*
** tpcc_txn.c - TPC-C transaction implementations
**
** Implements the three core TPC-C transactions:
**   - New Order
**   - Payment
**   - Order Status
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"
#include "tpcc_schema.h"

/* Transaction result structure */
typedef struct {
  int success;
  char error_msg[256];
} txn_result_t;

/*
** NEW ORDER Transaction (TPC-C 2.4)
**
** Creates a new order with 5-15 order lines
*/
txn_result_t tpcc_new_order_txn(sqlite3 *db, int w_id, int num_warehouses) {
  txn_result_t result = {0};
  sqlite3_stmt *stmt = NULL;
  int rc;
  
  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  int ol_cnt = rand() % 11 + 5; /* 5-15 order lines */
  int o_id = 0;
  
  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }
  
  /* Get next order ID for this district */
  const char *sql = "SELECT d_next_o_id FROM district WHERE d_w_id = ? AND d_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(result.error_msg, sizeof(result.error_msg), "Prepare failed: %s", sqlite3_errmsg(db));
    return result;
  }
  
  sqlite3_bind_int(stmt, 1, w_id);
  sqlite3_bind_int(stmt, 2, d_id);
  
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    o_id = sqlite3_column_int(stmt, 0);
  } else {
    const char *errmsg = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(result.error_msg, sizeof(result.error_msg), 
             "District lookup failed (w=%d,d=%d): rc=%d %s", w_id, d_id, rc, errmsg);
    return result;
  }
  sqlite3_finalize(stmt);
    
  /* Update district's next order ID */
  sql = "UPDATE district SET d_next_o_id = ? WHERE d_w_id = ? AND d_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, o_id + 1);
    sqlite3_bind_int(stmt, 2, w_id);
    sqlite3_bind_int(stmt, 3, d_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Insert new order */
  sql = "INSERT INTO orders VALUES (?, ?, ?, ?, datetime('now'), 0, ?, 1)";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, o_id);
    sqlite3_bind_int(stmt, 2, d_id);
    sqlite3_bind_int(stmt, 3, w_id);
    sqlite3_bind_int(stmt, 4, c_id);
    sqlite3_bind_int(stmt, 5, ol_cnt);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Insert into new_order table */
  sql = "INSERT INTO new_order VALUES (?, ?, ?)";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, o_id);
    sqlite3_bind_int(stmt, 2, d_id);
    sqlite3_bind_int(stmt, 3, w_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Process each order line */
  for (int ol_num = 1; ol_num <= ol_cnt; ol_num++) {
    int i_id = rand() % TPCC_NUM_ITEMS + 1;
    int supply_w_id = w_id; /* Simplified: always home warehouse */
    int quantity = rand() % 10 + 1;
    double amount = 0.0;
    
    /* Get item price */
    sql = "SELECT i_price FROM item WHERE i_id = ?";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, i_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        amount = sqlite3_column_double(stmt, 0) * quantity;
      }
      sqlite3_finalize(stmt);
    }
    
    /* Update stock */
    sql = "UPDATE stock SET s_quantity = s_quantity - ?, s_ytd = s_ytd + ?, "
          "s_order_cnt = s_order_cnt + 1 WHERE s_w_id = ? AND s_i_id = ?";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, quantity);
      sqlite3_bind_int(stmt, 2, quantity);
      sqlite3_bind_int(stmt, 3, supply_w_id);
      sqlite3_bind_int(stmt, 4, i_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    
    /* Insert order line */
    sql = "INSERT INTO order_line VALUES (?, ?, ?, ?, ?, ?, '', ?, ?, 'dist_info')";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, o_id);
      sqlite3_bind_int(stmt, 2, d_id);
      sqlite3_bind_int(stmt, 3, w_id);
      sqlite3_bind_int(stmt, 4, ol_num);
      sqlite3_bind_int(stmt, 5, i_id);
      sqlite3_bind_int(stmt, 6, supply_w_id);
      sqlite3_bind_int(stmt, 7, quantity);
      sqlite3_bind_double(stmt, 8, amount);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "COMMIT failed: %s", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return result;
  }
  
  result.success = 1;
  return result;
}

/*
** PAYMENT Transaction (TPC-C 2.5)
**
** Records a customer payment
*/
txn_result_t tpcc_payment_txn(sqlite3 *db, int w_id, int num_warehouses) {
  txn_result_t result = {0};
  sqlite3_stmt *stmt = NULL;
  int rc;
  
  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  double h_amount = (rand() % 500000 + 100) / 100.0; /* $1.00 to $5000.00 */
  
  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }
  
  /* Update warehouse */
  const char *sql = "UPDATE warehouse SET w_ytd = w_ytd + ? WHERE w_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_double(stmt, 1, h_amount);
    sqlite3_bind_int(stmt, 2, w_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Update district */
  sql = "UPDATE district SET d_ytd = d_ytd + ? WHERE d_w_id = ? AND d_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_double(stmt, 1, h_amount);
    sqlite3_bind_int(stmt, 2, w_id);
    sqlite3_bind_int(stmt, 3, d_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Update customer */
  sql = "UPDATE customer SET c_balance = c_balance - ?, c_ytd_payment = c_ytd_payment + ?, "
        "c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_double(stmt, 1, h_amount);
    sqlite3_bind_double(stmt, 2, h_amount);
    sqlite3_bind_int(stmt, 3, w_id);
    sqlite3_bind_int(stmt, 4, d_id);
    sqlite3_bind_int(stmt, 5, c_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  /* Insert history record */
  sql = "INSERT INTO history VALUES (?, ?, ?, ?, ?, datetime('now'), ?, 'Payment')";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, c_id);
    sqlite3_bind_int(stmt, 2, d_id);
    sqlite3_bind_int(stmt, 3, w_id);
    sqlite3_bind_int(stmt, 4, d_id);
    sqlite3_bind_int(stmt, 5, w_id);
    sqlite3_bind_double(stmt, 6, h_amount);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "COMMIT failed: %s", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return result;
  }
  
  result.success = 1;
  return result;
}

/*
** ORDER STATUS Transaction (TPC-C 2.6)
**
** Queries the status of a customer's most recent order (read-only)
*/
txn_result_t tpcc_order_status_txn(sqlite3 *db, int w_id, int num_warehouses) {
  txn_result_t result = {0};
  sqlite3_stmt *stmt = NULL;
  int rc;
  
  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  int o_id = 0;
  
  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }
  
  /* Get customer info */
  const char *sql = "SELECT c_balance, c_first, c_middle, c_last FROM customer "
                    "WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(result.error_msg, sizeof(result.error_msg), "Customer prepare: %s", sqlite3_errmsg(db));
    return result;
  }
  
  sqlite3_bind_int(stmt, 1, w_id);
  sqlite3_bind_int(stmt, 2, d_id);
  sqlite3_bind_int(stmt, 3, c_id);
  
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    const char *errmsg = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(result.error_msg, sizeof(result.error_msg), 
             "Customer not found (w=%d,d=%d,c=%d): rc=%d %s", w_id, d_id, c_id, rc, errmsg);
    return result;
  }
  sqlite3_finalize(stmt);
  
  /* Get most recent order for customer */
  sql = "SELECT o_id, o_entry_d, o_carrier_id FROM orders "
        "WHERE o_w_id = ? AND o_d_id = ? AND o_c_id = ? "
        "ORDER BY o_id DESC LIMIT 1";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, w_id);
    sqlite3_bind_int(stmt, 2, d_id);
    sqlite3_bind_int(stmt, 3, c_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      o_id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  
  if (o_id == 0) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(result.error_msg, sizeof(result.error_msg), "No orders found");
    return result;
  }
  
  /* Get order lines for this order */
  sql = "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d "
        "FROM order_line WHERE ol_w_id = ? AND ol_d_id = ? AND ol_o_id = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, w_id);
    sqlite3_bind_int(stmt, 2, d_id);
    sqlite3_bind_int(stmt, 3, o_id);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      /* Just fetch data - this is a read-only query */
    }
    sqlite3_finalize(stmt);
  }
  
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "COMMIT failed: %s", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return result;
  }
  
  result.success = 1;
  return result;
}
