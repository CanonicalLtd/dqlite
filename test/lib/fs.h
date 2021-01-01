#ifndef DQLITE_TEST_FS_H
#define DQLITE_TEST_FS_H

#define TEST_DIR_TEMPLATE "/tmp/dqlite-test-XXXXXX"

/* Setup a temporary directory. */
char *testDirSetup(void);

/* Remove the temporary directory. */
void testDirTearDown(char *dir);

#endif /* DQLITE_TEST_FS_H */
