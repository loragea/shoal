</$objtype/mkfile

# lib must be built before anything that links against it.
DIRS=lib cmd

default:V:	all

all:V:
	for(i in $DIRS) @{
		cd $i
		mk $MKFLAGS all
	}

clean nuke:V:
	for(i in $DIRS) @{
		cd $i
		mk $MKFLAGS $target
	}
