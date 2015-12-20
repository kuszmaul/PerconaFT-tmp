/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"

#include "toku_os.h"
#include "cachetable/checkpoint.h"

#define ENVDIR TOKU_TEST_FILENAME
#include "test-ft-txns.h"

static void do_txn(TOKULOGGER logger, bool readonly) {
    int r;
    TOKUTXN txn;
    r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_NONE, false);
    CKERR(r);

    if (!readonly) {
        toku_maybe_log_begin_txn_for_write_operation(txn);
    }
    r = toku_txn_commit_txn(txn, false, NULL, NULL);
    CKERR(r);

    toku_txn_close_txn(txn);
}

static void test_xid_lsn_independent(int N) {
    TOKULOGGER logger;
    CACHETABLE ct;
    int r;

    test_setup(TOKU_TEST_FILENAME, &logger, &ct);

    FT_HANDLE ft;

    TOKUTXN txn;
    r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_NONE, false);
    CKERR(r);

    r = toku_open_ft_handle("ftfile", 1, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, toku_builtin_compare_fun);
    CKERR(r);

    r = toku_txn_commit_txn(txn, false, NULL, NULL);
    CKERR(r);
    toku_txn_close_txn(txn);

    r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn, logger, TXN_SNAPSHOT_NONE, false);
    CKERR(r);
    TXNID xid_first = txn->txnid.parent_id64;
    unsigned int rands[N];
    for (int i=0; i<N; i++) {
        char key[100],val[300];
        DBT k, v;
        rands[i] = random();
        snprintf(key, sizeof(key), "key%x.%x", rands[i], i);
        memset(val, 'v', sizeof(val));
        val[sizeof(val)-1]=0;
        toku_ft_insert(ft, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v, val, 1+strlen(val)), txn);
    }
    {
        TOKUTXN txn2;
        r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn2, logger, TXN_SNAPSHOT_NONE, false);
    CKERR(r);
        // Verify the txnid has gone up only by one (even though many log entries were done)
        invariant(txn2->txnid.parent_id64 == xid_first + 1);
        r = toku_txn_commit_txn(txn2, false, NULL, NULL);
    CKERR(r);
        toku_txn_close_txn(txn2);
    }
    r = toku_txn_commit_txn(txn, false, NULL, NULL);
    CKERR(r);
    toku_txn_close_txn(txn);
    {
        //TODO(yoni) #5067 will break this portion of the test. (End ids are also assigned, so it would increase by 4 instead of 2.)
        // Verify the txnid has gone up only by two (even though many log entries were done)
        TOKUTXN txn3;
        r = toku_txn_begin_txn((DB_TXN*)NULL, (TOKUTXN)0, &txn3, logger, TXN_SNAPSHOT_NONE, false);
    CKERR(r);
        invariant(txn3->txnid.parent_id64 == xid_first + 2);
        r = toku_txn_commit_txn(txn3, false, NULL, NULL);
    CKERR(r);
        toku_txn_close_txn(txn3);
    }
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    r = toku_checkpoint(cp, logger, CLIENT_CHECKPOINT);
    CKERR(r);
    r = toku_close_ft_handle_nolsn(ft, NULL);
    CKERR(r);

    clean_shutdown(&logger, &ct);
}

static TXNID
logger_get_last_xid(TOKULOGGER logger) {
    TXN_MANAGER mgr = toku_logger_get_txn_manager(logger);
    return toku_txn_manager_get_last_xid(mgr);
}

static void test_xid_lsn_independent_crash_recovery(int N) {
    TOKULOGGER logger;
    CACHETABLE ct;
    int r;

    test_setup(TOKU_TEST_FILENAME, &logger, &ct);

    for (int i=0; i < N - 1; i++) {
        do_txn(logger, true);
    }
    do_txn(logger, false);

    TXNID last_xid_before = logger_get_last_xid(logger);

    toku_logger_close_rollback(logger);

    toku_cachetable_close(&ct);
    // "Crash"
    r = toku_logger_close(&logger);
    CKERR(r);
    ct = NULL;
    logger = NULL;

    // "Recover"
    test_setup_and_recover(TOKU_TEST_FILENAME, &logger, &ct);

    TXNID last_xid_after = logger_get_last_xid(logger);

    invariant(last_xid_after == last_xid_before);

    shutdown_after_recovery(&logger, &ct);
}

static void test_xid_lsn_independent_shutdown_recovery(int N) {
    TOKULOGGER logger;
    CACHETABLE ct;
    test_setup(TOKU_TEST_FILENAME, &logger, &ct);

    for (int i=0; i < N - 1; i++) {
        do_txn(logger, true);
    }
    do_txn(logger, false);

    TXNID last_xid_before = logger_get_last_xid(logger);

    clean_shutdown(&logger, &ct);

    // Did a clean shutdown.

    // "Recover"
    test_setup_and_recover(TOKU_TEST_FILENAME, &logger, &ct);

    TXNID last_xid_after = logger_get_last_xid(logger);

    invariant(last_xid_after == last_xid_before);

    shutdown_after_recovery(&logger, &ct);
}

static void test_xid_lsn_independent_parents(int N) {
    TOKULOGGER logger;
    CACHETABLE ct;
    int r;

    // Lets txns[-1] be NULL
    TOKUTXN txns_hack[N+1];
    TOKUTXN *txns=&txns_hack[1];

    int num_non_cascade = N;
    do {
        test_setup(TOKU_TEST_FILENAME, &logger, &ct);
        ZERO_ARRAY(txns_hack);

        for (int i = 0; i < N; i++) {
            r = toku_txn_begin_txn((DB_TXN*)NULL, txns[i-1], &txns[i], logger, TXN_SNAPSHOT_NONE, false);
            CKERR(r);

            if (i < num_non_cascade) {
                toku_maybe_log_begin_txn_for_write_operation(txns[i]);
                invariant(txns[i]->begin_was_logged);
            }
            else {
                invariant(!txns[i]->begin_was_logged);
            }
        }
        for (int i = 0; i < N; i++) {
            if (i < num_non_cascade) {
                toku_maybe_log_begin_txn_for_write_operation(txns[i]);
                invariant(txns[i]->begin_was_logged);
            }
            else {
                invariant(!txns[i]->begin_was_logged);
            }
        }
        toku_maybe_log_begin_txn_for_write_operation(txns[N-1]);
        for (int i = 0; i < N; i++) {
            invariant(txns[i]->begin_was_logged);
        }
        for (int i = N-1; i >= 0; i--) {
            r = toku_txn_commit_txn(txns[i], false, NULL, NULL);
            CKERR(r);

            toku_txn_close_txn(txns[i]);
        }
        clean_shutdown(&logger, &ct);

        num_non_cascade /= 2;
    } while (num_non_cascade > 0);
}

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    for (int i=1; i<=128; i *= 2) {
        test_xid_lsn_independent(i);
        test_xid_lsn_independent_crash_recovery(i);
        test_xid_lsn_independent_shutdown_recovery(i);
        test_xid_lsn_independent_parents(i);
    }
    return 0;
}
