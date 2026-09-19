/*
 * Private to mon/.  The two tables here — the file table and the ctl
 * verb table — are the contract the rest of the monitor's 9P surface
 * is added to: a file grows handler pointers in its own row, a verb is
 * a row of its own, and neither touches the other's lines.
 */

typedef struct Mfid Mfid;
typedef struct Mfile Mfile;
typedef struct Mctl Mctl;
typedef struct Mdirent Mdirent;
typedef struct Mseen Mseen;

/*
 * layer-a §8.1's tree, one entry per file, in the order it is listed
 * there.  Qmroot is the root directory; Qmaps is the `/maps'
 * directory and Qmapfile is the row with no fixed name — /maps/<epoch>
 * — which a walk reaches through it and which carries the same role
 * matrix and handler cells as every other row.
 */
enum
{
	Qmroot	= 0,
	Qctl,
	Qmap,
	Qmapnext,
	Qmaps,
	Qinstances,
	Qmstale,
	Qhealth,
	Qmstatus,
	Qmapfile,
	Nmfile,
};

/*
 * qid.path.  A /maps/<epoch> entry takes the published map's `seq' —
 * one seq space for the whole slot store, strictly increasing, never
 * reused, and 1 at the first publish (store.md §10) — so two entries
 * of the tree always differ and a path is stable for as long as the
 * entry is retained.  The fixed files take paths above every value a
 * seq can reach in this design.  store.md §14(55) records both, and
 * why an epoch is the wrong value to use: layer-a §8.3's `forceepoch'
 * and §8.6's rebuild path can publish one epoch twice.
 */
enum
{
	Pmfixed	= 1ULL<<63,	/* Pmfixed+Q<name> for the fixed files */
};

/*
 * One file of §8.1.  walk/rd/wr are §8.1's role matrix: which roles
 * may walk to this file, open it for reading, and open it for writing.
 * §8.1 states the matrix by role and leaves several cells unstated;
 * store.md §14(53) records what those cells are here and why.
 *
 * The handler cells are what the rest of the surface fills in.  Each
 * row of the table names one field per line, so a field added to this
 * struct touches no existing row.
 *
 *	render	the file is a render-at-open text file (§8.1's MUST for
 *		every status file): open composes its bytes once into the
 *		fid's Mtext and every read is served from them.  It
 *		answers nil, or an error string.  It runs under the slot
 *		store's lock, which monsrvopen takes around it.
 *	read	a read that is not served from a snapshot — the two
 *		directory reads.  It responds.
 *	write	a write.  It responds.
 *	open	a file whose open takes checks of its own.  It responds.
 *
 * A nil handler cell is an operation whose content is not built:
 * after the role gate it answers the local Emonnotbuilt.  That is
 * deliberate, so the gate matrix is complete and testable before the
 * content is — /map.next is exactly such a row in this unit.
 */
struct Mfile
{
	char	*name;		/* nil for the /maps/<epoch> row */
	int	isdir;
	ulong	perm;		/* the mode stat and open report */
	int	walk;
	int	rd;
	int	wr;
	char*	(*render)(Monctx*, Mfid*, Mtext*);
	void	(*read)(Req*);
	void	(*write)(Req*);
	void	(*open)(Req*);
};

extern Mfile monfiles[Nmfile];

/* one entry of the /maps listing, snapshotted at the directory's open */
struct Mdirent
{
	uvlong	epoch;
	uvlong	seq;
};

/*
 * Per-fid state.  A fid is a file of the table plus, for the
 * /maps/<epoch> row, the epoch it named; the attach attributes ride on
 * every fid derived from that attach (layer-a §8.1: the role and the
 * peer travel in aname and a walk clones them), which is what lets
 * §8.4's evidence rule be stated per fid — the /map fid whose read
 * counts is one whose ATTACH carried role=instance,peer=i.
 *
 * `mapseq' is the other half of §8.4's evidence rule: a /map fid
 * carries the `seq' of the map its snapshot was copied from, and a
 * read renews lastseen only while that is still the current map's
 * (store.md §14(51)).  It is stamped by the render, under monlk, so
 * it and the bytes are one state.
 *
 * Nothing a fid holds outlives the slot store: `text' is a copy of the
 * bytes, made under the lock at open, and `dir' is a copy of the ring
 * positions.  So no fid needs a close hook and this service keeps no
 * registry of its live fids — the difference from srv/, where a fid
 * can hold an engine handle that MUST be given back before the store
 * closes (srv/dat.h).  A fid here is freed by destroyfid whenever
 * lib9p gets to it, before or after the shutdown, and both orders are
 * the same order.
 */
struct Mfid
{
	int	file;		/* index into monfiles */
	int	role;
	char	peer[Iidlen+1];	/* role=instance's peer= */
	uvlong	epoch;		/* Qmapfile: the epoch this fid names */
	uvlong	qidpath;
	uvlong	qidvers;
	Mtext	*text;		/* the render-at-open snapshot, once open */
	uvlong	mapseq;		/* Qmap: the seq of the map `text' copied */
	Mdirent	*dir;		/* Qmroot, Qmaps: the listing snapshot */
	int	ndir;
};

/*
 * One verb of layer-a §8.3.  `roles' is the verb's Role column, which
 * §8.3 states per row: the instance-reported verbs are role=instance
 * and the operator verbs are role=admin, and no verb of either table
 * is both.  The argument counts are the verb's Form column.
 *
 * `fn' is the verb's body and is what the next two units fill in: this
 * unit's whole ctl surface is the framework around it, so every row's
 * fn is ctlnotbuilt and the effect column of §8.3 is unreached.  The
 * unit that builds /map.next and the operator verbs fills the second
 * table's rows and `register'; the unit after it fills the rest of the
 * first table.  A verb is built by filling one cell.
 */
struct Mctl
{
	char	*verb;
	int	roles;
	int	nargmin;
	int	nargmax;
	char*	(*fn)(Monctx*, Mfid*, int, char**);
};

extern Mctl monctls[];
extern int nmonctls;

/* one instance's §8.4 evidence: lastseen(iid), in seconds */
struct Mseen
{
	char	iid[Iidlen+1];
	uvlong	t;
};

/*
 * The service's own state.
 *
 * `monlk' covers the slot store and the parsed current map, and is
 * what orders every render against a publish (mon.h).  `seenlk'
 * covers the liveness registry alone and is taken under `monlk' by the
 * two renders that need both, never the other way round.  `statelk'
 * covers the four shutdown flags and is a leaf.
 *
 * The staged map of §8.1's /map.next, and the `pending=' count
 * /status reports for it, are the next unit's and belong here beside
 * the current map: they are state of the service, guarded by `monlk'
 * like the map they are staged against, and /status renders
 * `pending=0' until they exist (store.md §14(56)).
 */
struct Monctx
{
	Srv	srv;
	Moncfg	cfg;
	Dev	*dev;

	QLock	monlk;
	Mon	*mon;
	Cmap	*map;		/* the parsed current map, nil when none */

	uvlong	t0;		/* start, seconds: /status's uptime= */

	Lock	seenlk;
	Mseen	*seen;
	int	nseen;
	int	maxseen;

	Lock	statelk;
	int	stopping;
	int	served;
	int	released;
	int	closed;
};
