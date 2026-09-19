</$objtype/mkfile

# lib must be built before anything that links against it.
DIRS=lib srv mon cmd test

default:V:	all

all:V:
	for(i in $DIRS) @{
		cd $i
		mk $MKFLAGS all
	}

# T1: build everything, then run the unit tests.
test:V:	all
	@{
		cd test
		mk $MKFLAGS test
	}

clean nuke:V:
	for(i in $DIRS) @{
		cd $i
		mk $MKFLAGS $target
	}
