#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <fcall.h>
#include "../lib/shoal.h"
#include "t1.h"

/*
 * T1 for the engine calls layer-a §5.5/§5.6's peer channels need.  Scaffold: the checks are added
 * with the code they test.
 */

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	killspawned();
	if(fails > 0){
		print("peeropstest: %d of %d checks FAILED\n", fails, checks);
		exits("failed");
	}
	print("peeropstest: %d checks ok\n", checks);
	exits(nil);
}
