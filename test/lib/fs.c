#include <ftw.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs.h"
#include "munit.h"

char *testDirSetup()
{
	char *dir = munit_malloc(strlen(TEST_DIR_TEMPLATE) + 1);

	strcpy(dir, TEST_DIR_TEMPLATE);

	munit_assert_ptr_not_null(mkdtemp(dir));

	return dir;
}

static int testDirTearDownNftwFn(const char *path,
				 const struct stat *sb,
				 int type,
				 struct FTW *ftwb)
{
	int rc;

	(void)sb;
	(void)type;
	(void)ftwb;

	rc = remove(path);
	munit_assert_int(rc, ==, 0);

	return 0;
}

void testDirTearDown(char *dir)
{
	int rc;

	rc = nftw(dir, testDirTearDownNftwFn, 10,
		  FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
	munit_assert_int(rc, ==, 0);
	free(dir);
}
