/*
** Simple interactive shell for sqlite4 with vector support.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "sqlite4.h"

#define MAX_LINE 65536

static int gHasError = 0;

extern void sqlite4_shell_timing_report(void);

static double shell_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static int callback(void *pArg, int nCol, sqlite4_value **apVal, const char **azCol) {
    int i;
    for (i = 0; i < nCol; i++) {
        int n = 0;
        const char *z = (const char*)sqlite4_value_text(apVal[i], &n);
        if (i > 0) printf("|");
        if (z) printf("%.*s", n, z);
        else   printf("NULL");
    }
    printf("\n");
    return 0;
}

static void print_header(sqlite4 *db, const char *sql) {
    /* Print column names via prepare */
    sqlite4_stmt *pStmt = NULL;
    if (sqlite4_prepare(db, sql, -1, &pStmt, 0) == SQLITE4_OK) {
        int i, n = sqlite4_column_count(pStmt);
        for (i = 0; i < n; i++) {
            if (i > 0) printf("|");
            printf("%s", sqlite4_column_name(pStmt, i));
        }
        if (n > 0) printf("\n");
        sqlite4_finalize(pStmt);
    }
}

static void run_sql(sqlite4 *db, const char *sql) {
    int rc;
    char *zErr = 0;

    /* Print column headers */
    print_header(db, sql);

    rc = sqlite4_exec(db, sql, callback, NULL);
    if (rc != SQLITE4_OK) {
        fprintf(stderr, "Error: %s\n", sqlite4_errmsg(db));
        gHasError = 1;
    }
}

static char *read_line(FILE *f, const char *prompt) {
    static char buf[MAX_LINE];
    if (prompt && isatty(fileno(f))) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if (!fgets(buf, sizeof(buf), f)) return NULL;
    return buf;
}

int main(int argc, char **argv) {
    sqlite4 *db = NULL;
    char accum[MAX_LINE * 4];
    const char *zFile = ":memory:";
    int rc;
    int interactive;

    if (argc >= 2) zFile = argv[1];

    rc = sqlite4_open(0, zFile, &db, 0);
    if (rc != SQLITE4_OK) {
        fprintf(stderr, "Cannot open database '%s': %d%s%s\n",
                zFile, rc, db ? ": " : "", db ? sqlite4_errmsg(db) : "");
        return 1;
    }

    interactive = isatty(fileno(stdin));

    if (interactive) {
        printf("SQLite4 + Vector Extension\n");
        printf("Connected to: %s\n", zFile);
        printf("Enter SQL statements terminated with ';'\n");
        printf("Type .quit to exit\n\n");
    }

    accum[0] = '\0';

    while (1) {
        const char *prompt = (accum[0] == '\0') ? "sqlite4> " : "      -> ";
        char *line = read_line(stdin, prompt);

        if (!line) break;  /* EOF */

        /* Trim newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Dot commands */
        if (accum[0] == '\0' && line[0] == '.') {
            if (strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) break;
            if (strcmp(line, ".help") == 0) {
                printf(".quit       Exit this program\n");
                printf(".exit       Exit this program\n");
                printf(".help       Show this help\n");
            } else {
                fprintf(stderr, "Unknown command: %s\n", line);
            }
            continue;
        }

        /* Accumulate SQL */
        if (strlen(accum) + len + 2 < sizeof(accum)) {
            if (accum[0]) strcat(accum, " ");
            strcat(accum, line);
        }

        /* Check if statement is complete (ends with ;) */
        const char *p = accum + strlen(accum) - 1;
        while (p > accum && (*p == ' ' || *p == '\t')) p--;
        if (*p == ';') {
            run_sql(db, accum);
            accum[0] = '\0';
            if (gHasError && !interactive) break;
        }
    }

    /* Run any remaining input */
    if (accum[0] && !gHasError) run_sql(db, accum);

    sqlite4_shell_timing_report();
    {
        double t0 = shell_now_ms();
        sqlite4_close(db, 0);
        fprintf(stderr, "shell db close: %.1f ms\n", shell_now_ms() - t0);
    }
    return gHasError ? 1 : 0;
}
