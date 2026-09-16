#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for docs/design/store.md §8's online bitmap rebuild (D18).  Scaffold: the checks are added
 * with the code they test.
 */

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	killspawned();
	if(fails > 0){
		print("bmrebuildtest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("bmrebuildtest: %d checks ok\n", checks);
	exits(nil);
}
