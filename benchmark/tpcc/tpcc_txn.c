/*
** tpcc_txn.c - TPC-C transaction implementations
**
** Implements the three core TPC-C transactions:
**   - New Order
**   - Payment
**   - Order Status
**
** All statements are prepared once via tpcc_prepare_stmts() and reused
** across transactions (reset+rebind), eliminating per-call parse overhead.
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

/* ── Prepared statement cache ─────────────────────────────────── */

typedef struct {
  /* New Order */
  sqlite3_stmt *no_get_dist;
  sqlite3_stmt *no_upd_dist;
  sqlite3_stmt *no_ins_order;
  sqlite3_stmt *no_ins_neworder;
  sqlite3_stmt *no_get_item;
  sqlite3_stmt *no_upd_stock;
  sqlite3_stmt *no_ins_ol;
  /* Payment */
  sqlite3_stmt *pay_upd_wh;
  sqlite3_stmt *pay_upd_dist;
  sqlite3_stmt *pay_upd_cust;
  sqlite3_stmt *pay_ins_hist;
  /* Order Status */
  sqlite3_stmt *os_get_cust;
  sqlite3_stmt *os_get_order;
  sqlite3_stmt *os_get_ol;
} tpcc_stmts_t;

static tpcc_stmts_t S;

static int prep(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
  int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
  if (rc != SQLITE_OK)
    fprintf(stderr, "prepare failed: %s\n  SQL: %s\n", sqlite3_errmsg(db), sql);
  return rc;
}

int tpcc_prepare_stmts(sqlite3 *db) {
  if (prep(db, "SELECT d_next_o_id FROM district WHERE d_w_id=? AND d_id=?",
           &S.no_get_dist)) return -1;
  if (prep(db, "UPDATE district SET d_next_o_id=? WHERE d_w_id=? AND d_id=?",
           &S.no_upd_dist)) return -1;
  if (prep(db, "INSERT INTO orders VALUES(?,?,?,?,datetime('now'),0,?,1)",
           &S.no_ins_order)) return -1;
  if (prep(db, "INSERT INTO new_order VALUES(?,?,?)",
           &S.no_ins_neworder)) return -1;
  if (prep(db, "SELECT i_price FROM item WHERE i_id=?",
           &S.no_get_item)) return -1;
  if (prep(db, "UPDATE stock SET s_quantity=s_quantity-?, s_ytd=s_ytd+?, "
           "s_order_cnt=s_order_cnt+1 WHERE s_w_id=? AND s_i_id=?",
           &S.no_upd_stock)) return -1;
  if (prep(db, "INSERT INTO order_line VALUES(?,?,?,?,?,?,'',?,?,'dist_info')",
           &S.no_ins_ol)) return -1;

  if (prep(db, "UPDATE warehouse SET w_ytd=w_ytd+? WHERE w_id=?",
           &S.pay_upd_wh)) return -1;
  if (prep(db, "UPDATE district SET d_ytd=d_ytd+? WHERE d_w_id=? AND d_id=?",
           &S.pay_upd_dist)) return -1;
  if (prep(db, "UPDATE customer SET c_balance=c_balance-?, c_ytd_payment=c_ytd_payment+?, "
           "c_payment_cnt=c_payment_cnt+1 WHERE c_w_id=? AND c_d_id=? AND c_id=?",
           &S.pay_upd_cust)) return -1;
  if (prep(db, "INSERT INTO history VALUES(?,?,?,?,?,datetime('now'),?,'Payment')",
           &S.pay_ins_hist)) return -1;

  if (prep(db, "SELECT c_balance,c_first,c_middle,c_last FROM customer "
           "WHERE c_w_id=? AND c_d_id=? AND c_id=?",
           &S.os_get_cust)) return -1;
  if (prep(db, "SELECT o_id,o_entry_d,o_carrier_id FROM orders "
           "WHERE o_w_id=? AND o_d_id=? AND o_c_id=? ORDER BY o_id DESC LIMIT 1",
           &S.os_get_order)) return -1;
  if (prep(db, "SELECT ol_i_id,ol_supply_w_id,ol_quantity,ol_amount,ol_delivery_d "
           "FROM order_line WHERE ol_w_id=? AND ol_d_id=? AND ol_o_id=?",
           &S.os_get_ol)) return -1;

  return 0;
}

void tpcc_finalize_stmts(void) {
  sqlite3_finalize(S.no_get_dist);
  sqlite3_finalize(S.no_upd_dist);
  sqlite3_finalize(S.no_ins_order);
  sqlite3_finalize(S.no_ins_neworder);
  sqlite3_finalize(S.no_get_item);
  sqlite3_finalize(S.no_upd_stock);
  sqlite3_finalize(S.no_ins_ol);
  sqlite3_finalize(S.pay_upd_wh);
  sqlite3_finalize(S.pay_upd_dist);
  sqlite3_finalize(S.pay_upd_cust);
  sqlite3_finalize(S.pay_ins_hist);
  sqlite3_finalize(S.os_get_cust);
  sqlite3_finalize(S.os_get_order);
  sqlite3_finalize(S.os_get_ol);
  memset(&S, 0, sizeof(S));
}

/* Helper: reset a cached statement for reuse */
static void use(sqlite3_stmt *s) { sqlite3_reset(s); sqlite3_clear_bindings(s); }

/*
** Reset all cached statements.  Must be called after every COMMIT or
** ROLLBACK so that no SELECT statement holds an open read-snapshot.
** In WAL mode, an unreset SELECT prevents the auto-checkpoint from
** advancing — the WAL grows without bound.
*/
static void reset_all(void) {
  sqlite3_reset(S.no_get_dist);
  sqlite3_reset(S.no_upd_dist);
  sqlite3_reset(S.no_ins_order);
  sqlite3_reset(S.no_ins_neworder);
  sqlite3_reset(S.no_get_item);
  sqlite3_reset(S.no_upd_stock);
  sqlite3_reset(S.no_ins_ol);
  sqlite3_reset(S.pay_upd_wh);
  sqlite3_reset(S.pay_upd_dist);
  sqlite3_reset(S.pay_upd_cust);
  sqlite3_reset(S.pay_ins_hist);
  sqlite3_reset(S.os_get_cust);
  sqlite3_reset(S.os_get_order);
  sqlite3_reset(S.os_get_ol);
}

/*
** NEW ORDER Transaction (TPC-C 2.4)
**
** Creates a new order with 5-15 order lines
*/
txn_result_t tpcc_new_order_txn(sqlite3 *db, int w_id, int num_warehouses) {
  txn_result_t result = {0};
  int rc;
  (void)num_warehouses;

  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  int ol_cnt = rand() % 11 + 5;
  int o_id = 0;

  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }

  /* Get next order ID for this district */
  use(S.no_get_dist);
  sqlite3_bind_int(S.no_get_dist, 1, w_id);
  sqlite3_bind_int(S.no_get_dist, 2, d_id);
  rc = sqlite3_step(S.no_get_dist);
  if (rc == SQLITE_ROW) {
    o_id = sqlite3_column_int(S.no_get_dist, 0);
  } else {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    reset_all();
    snprintf(result.error_msg, sizeof(result.error_msg),
             "District lookup failed (w=%d,d=%d): rc=%d %s", w_id, d_id, rc, sqlite3_errmsg(db));
    return result;
  }

  /* Update district's next order ID */
  use(S.no_upd_dist);
  sqlite3_bind_int(S.no_upd_dist, 1, o_id + 1);
  sqlite3_bind_int(S.no_upd_dist, 2, w_id);
  sqlite3_bind_int(S.no_upd_dist, 3, d_id);
  sqlite3_step(S.no_upd_dist);

  /* Insert new order */
  use(S.no_ins_order);
  sqlite3_bind_int(S.no_ins_order, 1, o_id);
  sqlite3_bind_int(S.no_ins_order, 2, d_id);
  sqlite3_bind_int(S.no_ins_order, 3, w_id);
  sqlite3_bind_int(S.no_ins_order, 4, c_id);
  sqlite3_bind_int(S.no_ins_order, 5, ol_cnt);
  sqlite3_step(S.no_ins_order);

  /* Insert into new_order table */
  use(S.no_ins_neworder);
  sqlite3_bind_int(S.no_ins_neworder, 1, o_id);
  sqlite3_bind_int(S.no_ins_neworder, 2, d_id);
  sqlite3_bind_int(S.no_ins_neworder, 3, w_id);
  sqlite3_step(S.no_ins_neworder);

  /* Process each order line */
  for (int ol_num = 1; ol_num <= ol_cnt; ol_num++) {
    int i_id = rand() % TPCC_NUM_ITEMS + 1;
    int supply_w_id = w_id;
    int quantity = rand() % 10 + 1;
    double amount = 0.0;

    /* Get item price */
    use(S.no_get_item);
    sqlite3_bind_int(S.no_get_item, 1, i_id);
    if (sqlite3_step(S.no_get_item) == SQLITE_ROW) {
      amount = sqlite3_column_double(S.no_get_item, 0) * quantity;
    }

    /* Update stock */
    use(S.no_upd_stock);
    sqlite3_bind_int(S.no_upd_stock, 1, quantity);
    sqlite3_bind_int(S.no_upd_stock, 2, quantity);
    sqlite3_bind_int(S.no_upd_stock, 3, supply_w_id);
    sqlite3_bind_int(S.no_upd_stock, 4, i_id);
    sqlite3_step(S.no_upd_stock);

    /* Insert order line */
    use(S.no_ins_ol);
    sqlite3_bind_int(S.no_ins_ol, 1, o_id);
    sqlite3_bind_int(S.no_ins_ol, 2, d_id);
    sqlite3_bind_int(S.no_ins_ol, 3, w_id);
    sqlite3_bind_int(S.no_ins_ol, 4, ol_num);
    sqlite3_bind_int(S.no_ins_ol, 5, i_id);
    sqlite3_bind_int(S.no_ins_ol, 6, supply_w_id);
    sqlite3_bind_int(S.no_ins_ol, 7, quantity);
    sqlite3_bind_double(S.no_ins_ol, 8, amount);
    sqlite3_step(S.no_ins_ol);
  }

  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  reset_all();
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
  int rc;
  (void)num_warehouses;

  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  double h_amount = (rand() % 500000 + 100) / 100.0;

  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }

  /* Update warehouse */
  use(S.pay_upd_wh);
  sqlite3_bind_double(S.pay_upd_wh, 1, h_amount);
  sqlite3_bind_int(S.pay_upd_wh, 2, w_id);
  sqlite3_step(S.pay_upd_wh);

  /* Update district */
  use(S.pay_upd_dist);
  sqlite3_bind_double(S.pay_upd_dist, 1, h_amount);
  sqlite3_bind_int(S.pay_upd_dist, 2, w_id);
  sqlite3_bind_int(S.pay_upd_dist, 3, d_id);
  sqlite3_step(S.pay_upd_dist);

  /* Update customer */
  use(S.pay_upd_cust);
  sqlite3_bind_double(S.pay_upd_cust, 1, h_amount);
  sqlite3_bind_double(S.pay_upd_cust, 2, h_amount);
  sqlite3_bind_int(S.pay_upd_cust, 3, w_id);
  sqlite3_bind_int(S.pay_upd_cust, 4, d_id);
  sqlite3_bind_int(S.pay_upd_cust, 5, c_id);
  sqlite3_step(S.pay_upd_cust);

  /* Insert history record */
  use(S.pay_ins_hist);
  sqlite3_bind_int(S.pay_ins_hist, 1, c_id);
  sqlite3_bind_int(S.pay_ins_hist, 2, d_id);
  sqlite3_bind_int(S.pay_ins_hist, 3, w_id);
  sqlite3_bind_int(S.pay_ins_hist, 4, d_id);
  sqlite3_bind_int(S.pay_ins_hist, 5, w_id);
  sqlite3_bind_double(S.pay_ins_hist, 6, h_amount);
  sqlite3_step(S.pay_ins_hist);

  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  reset_all();
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
  int rc;
  (void)num_warehouses;

  int d_id = rand() % TPCC_DISTRICTS_PER_WH + 1;
  int c_id = rand() % TPCC_CUSTOMERS_PER_DIST + 1;
  int o_id = 0;

  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "BEGIN failed: %s", sqlite3_errmsg(db));
    return result;
  }

  /* Get customer info */
  use(S.os_get_cust);
  sqlite3_bind_int(S.os_get_cust, 1, w_id);
  sqlite3_bind_int(S.os_get_cust, 2, d_id);
  sqlite3_bind_int(S.os_get_cust, 3, c_id);
  rc = sqlite3_step(S.os_get_cust);
  if (rc != SQLITE_ROW) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    reset_all();
    snprintf(result.error_msg, sizeof(result.error_msg),
             "Customer not found (w=%d,d=%d,c=%d): rc=%d %s", w_id, d_id, c_id, rc, sqlite3_errmsg(db));
    return result;
  }

  /* Get most recent order for customer */
  use(S.os_get_order);
  sqlite3_bind_int(S.os_get_order, 1, w_id);
  sqlite3_bind_int(S.os_get_order, 2, d_id);
  sqlite3_bind_int(S.os_get_order, 3, c_id);
  if (sqlite3_step(S.os_get_order) == SQLITE_ROW) {
    o_id = sqlite3_column_int(S.os_get_order, 0);
  }

  if (o_id == 0) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    reset_all();
    snprintf(result.error_msg, sizeof(result.error_msg), "No orders found");
    return result;
  }

  /* Get order lines for this order */
  use(S.os_get_ol);
  sqlite3_bind_int(S.os_get_ol, 1, w_id);
  sqlite3_bind_int(S.os_get_ol, 2, d_id);
  sqlite3_bind_int(S.os_get_ol, 3, o_id);
  while (sqlite3_step(S.os_get_ol) == SQLITE_ROW) {
    /* Just fetch data - this is a read-only query */
  }

  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  reset_all();
  if (rc != SQLITE_OK) {
    snprintf(result.error_msg, sizeof(result.error_msg), "COMMIT failed: %s", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return result;
  }

  result.success = 1;
  return result;
}
