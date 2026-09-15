/*
 * libshoal's store engine — internal structures, docs/design/store.md
 * §3 to §7.  This header is private to lib/: everything a command, a
 * server or a test may touch is in shoal.h.  Include after <u.h>,
 * <libc.h>, <libsec.h>, <fcall.h> and "shoal.h".
 *
 * The engine uses QLock, Rendez and Lock and nothing else, so that
 * one build of it runs both in a plain-libc T1 program whose procs
 * are rfork(RFPROC|RFMEM) and in the libthread 9P server whose procs
 * are proccreate: both give libc's qlock(2) primitives their meaning.
 * Procs are made through the caller's spawn callback (§7).
 */

typedef struct Ient Ient;
typedef struct Emape Emape;
typedef struct Item Item;
typedef struct Batch Batch;
typedef struct Peer Peer;
typedef struct Omap Omap;
typedef struct Sgrain Sgrain;

/*
 * The staged set, §6: the grains stages have reserved and no record
 * has yet named.  A hash of grain numbers rather than a second bit
 * array over the whole disk, because a few thousand reservations is
 * what it ever holds.
 */
struct Sgrain
{
	ulong	g;
	Sgrain	*next;
};

/*
 * The in-memory index entry, §9.  There is no lock in it: §7's queues
 * serialise per object, so the bytes an RWLock costs in every slot,
 * occupied or not, are not spent.  §9 keeps the oids in one arena;
 * they are separate allocations here, which is the same bytes with an
 * allocator that can also give them back when a slot is discarded.
 */
struct Ient
{
	uvlong	qidpath;
	uvlong	len;
	uvlong	ver;
	uvlong	wepoch;
	vlong	mtime;
	uchar	csum[Csumlen];
	uchar	dig0[Blkdlen];		/* inline map: digest of block 0 */
	uchar	*oid;			/* oidlen bytes */
	ulong	emapslot;
	ulong	grain0;			/* inline map: 0 = hole */
	uvlong	cur;			/* R5: in memory only, never on disk */
	ulong	hashnext;
	uchar	oidlen;
	uchar	oidcap;			/* bytes oid holds; §3.2's pre-allocation */
	uchar	state;
	uchar	flags;
	uchar	bad;			/* condemned by §5 step 10 */
};

/*
 * An extent-map cache entry, §9.  A pinned entry is not in the
 * eviction set and its bytes are the pin holder's to mutate: that is
 * what lets a commit apply its whole batch under qlstate alone
 * without ever reading the device under a lock (§7).  busy covers
 * both a load and a write-back; a proc that finds one waits.
 */
struct Emape
{
	ulong	slot;
	int	busy;
	int	pin;
	int	dirty;
	int	wb;			/* the checkpointer is writing it back */
	int	bad;			/* the on-disk entry failed its csum */
	uchar	*p;			/* emapsz bytes */
	Emape	*hnext;
	Emape	*prev, *next;		/* LRU, most recent first */
	Emape	*dnext;			/* the dirty list, under qlstate */
};

/*
 * One operation's contribution to a batch (§7).  A committer takes
 * the pending queue's items, writes one record carrying all of their
 * entries, and applies them all.
 */
enum
{
	Ipending	= 0,
	Ibatched,
	Idone,
};

struct Item
{
	int	state;
	int	freeing;		/* draws on §6's reserved log tail */
	Objrec	*obj;			/* one Eobj, or nil */
	Objrec	objb;			/* ... which a queued item holds here */
	Dirtyrec *dirty;		/* ndirty Edirty entries */
	int	ndirty;
	ulong	eslot;			/* an Eslot's slot */
	int	haseslot;
	ulong	nbyte;			/* entry bytes this item contributes */
	Dirtent	**spare;		/* ndirty pre-allocated dirty records */
	Emape	*emap;			/* the pinned map the apply mutates */
	char	err[ERRMAX];
	Item	*next;
};

struct Batch
{
	uvlong	seqlo, seqhi;		/* a wrap record takes seqlo */
	uvlong	off;			/* log sector of the header */
	uvlong	wrapoff;		/* log sector of the wrap record */
	uvlong	endoff;			/* log sector after the record */
	ulong	nsec;
	int	wrap;
	Item	*items;
};

/* a peer's coarse sync state, §2.6.  fullsync is never persisted. */
struct Peer
{
	char	name[Peermax+1];
	int	fullsync;
	Peer	*next;
};

/*
 * A block map, uniformly: the index entry's inline grain0/dig0 for a
 * one-block object and the extent-map entry otherwise (§2.3).  c is
 * pinned for as long as the Omap is used.
 */
struct Omap
{
	Store	*s;
	Ient	*e;
	Emape	*c;
};

/*
 * A directory snapshot, §9.  The two parallel arrays are the vector
 * of {slot, qid.path} §9 sizes at 12 bytes an entry, taken under one
 * hold of qlstate at open; kinds is the set of states that open asked
 * for, and is the other half of what makes an entry gone.  Nothing
 * here is a reference the engine must honour: the slot may be freed,
 * reused or re-stated under it, which is exactly what the two tests
 * in objsnapent detect.
 */
struct Objsnap
{
	Store	*s;
	int	kinds;
	ulong	n;
	ulong	*slot;
	uvlong	*qidpath;
};

struct Store
{
	Dev	*d;
	Super	sb;			/* the geometry; §2.2's five
					 * publishable fields live in pub */
	Storecfg cfg;
	int	flushmode;
	uchar	*zeroblk;		/* blksz zero bytes */
	uchar	zerodig[Blkdlen];	/* the full-block zero digest */

	/*
	 * §7's four state locks.  No proc holds two of them at once;
	 * fllk, cklk and proclk below are leaves.
	 */
	QLock	qlstate;
	QLock	qlemap;
	QLock	qllog;
	QLock	qlsuper;

	/* the in-memory index and its hash table, §9 */
	Ient	*idx;
	ulong	*hash;
	ulong	nhash;
	ulong	nlive, ntomb;
	ulong	nobjsnap;		/* §9's open snapshots; qlstate's */
	/*
	 * §9: storeclose has run.  It is the store's OWN reference, not
	 * a flag beside one — the memory goes when `closed && nobjsnap
	 * == 0', tested under qlstate by storeclose and by every
	 * objsnapclose, so there is no second counter to drift out of
	 * step with the bound's.
	 */
	int	closed;			/* qlstate's */
	ulong	snapstale;		/* §13's snapstale point; qlstate's */
	ulong	snapshort;		/* ... by how many entries; qlstate's */
	/*
	 * §13's snaphold point: park one objsnapopen with the bound's
	 * slot taken until storeclose has set `closed', so a test can
	 * drive the window in which that open holds the store's last
	 * claim instead of racing for it.  Cleared by the open that
	 * takes it, so one arming parks one open.
	 */
	int	snaphold;		/* qlstate's */
	Rendez	snaprz;			/* on qlstate: the open parked there */

	/* the two slot spaces, §6.  resv is a stage's reservation. */
	uchar	*slotused;
	uchar	*slotresv;
	ulong	slotcur, slotfree;
	uchar	*emapused;
	uchar	*emapresv;
	ulong	emapcur, emapfree;

	/* the free-grain bitmap and the staged set, §6 */
	uchar	*bmap;
	uchar	*bmdirty;		/* a byte per bitmap page */
	uvlong	nbmpage;
	uvlong	graincur;
	uvlong	grainfree;
	Sgrain	**stagebuck;		/* §6's hash of reserved grains */
	Sgrain	*stagefree;		/* recycled nodes */
	ulong	nstagebuck;
	ulong	nstaged;

	/* dirty index and dirty-record pages, for the checkpointer */
	uchar	*idxdirty;
	ulong	nidxpage;
	uchar	*dirtdirty;
	ulong	ndirtpage;

	/* the dirty set, §2.6 */
	Dirtent	**dirt;			/* ndirty slots, nil where free */
	ulong	ndirtused;
	Peer	*peers;

	/* the extent-map cache, §9 */
	Emape	**ehash;
	ulong	nehash;
	Emape	*elru, *elrutail;
	Emape	*edirty;		/* dirty entries, under qlstate */
	ulong	nemapc, emapcap;
	Rendez	emaprz;			/* on qlemap */

	/* the log and group commit, §3.2 and §7 */
	uvlong	seqnext;
	uvlong	relseq;			/* highest seq released, §7 */
	uvlong	watermark;		/* highest durably applied seq */
	uvlong	wateroff;		/* log sector after its record */
	uvlong	logtail;		/* sector, relative to logoff */
	uvlong	logstart;		/* the reclaim point, relative */
	uvlong	logresv;		/* §6's reserved tail, sectors */
	ulong	logdepth, nflight;
	Item	*pend, *pendtail;
	Rendez	roomrz;			/* on qllog: room, or absorbed */
	Rendez	relrz;			/* on qllog: the release order moved */
	Rendez	donerz;			/* on qllog: a batch completed */
	Rendez	holdrz;			/* on qllog: §13's batch:n hold */
	uvlong	holdseq;
	uvlong	nlogwait;		/* commits parked in §6's wait */
	int	broken;			/* a log write failed: commit no more */
	uvlong	failseq;		/* the first batch that did not land */
	char	failerr[ERRMAX];	/* the device error that broke it */
	int	fatal;			/* memory no longer matches the log;
					 * qllog's, like broken beside it,
					 * however the apply that sets it runs */
	int	reclaimearly;		/* §13's reclaim point */
	uvlong	pubatpage;		/* §13's publish point */
	int	fullwait;		/* §13's fullwait point */

	/* the flusher, §3.2: one flush satisfies every waiter */
	QLock	fllk;
	Rendez	flrz;			/* on fllk: a flush completed */
	uvlong	flasked, fldone;
	uvlong	flhold, flcount;	/* §13's flush:n hold */
	int	flerr, flbusy, flproc;

	/* the checkpointer, §2.8 */
	QLock	cklk;
	Rendez	ckrz;			/* on cklk: a checkpoint completed */
	uvlong	ckreq, ckdone;
	uvlong	ckforce, ckfdone;	/* requests from storecheckpoint alone:
					 * §2.8's retry floor does not pace
					 * them, and ckreq cannot tell them
					 * from a committer's ask */
	int	ckret, ckbusy, ckproc;	/* ckret: the last one's return */
	int	ckstuck;		/* the LAST checkpoint failed */
	ulong	ckbackms;		/* the floor in force, doubling */
	vlong	ckwake;			/* no paced attempt before this nsec */
	uvlong	ckfailed;		/* checkpoint attempts that failed */
	char	ckerrstr[ERRMAX];	/* what the last failure said */
	vlong	cklast;
	uvlong	ndirtypage;		/* pages dirtied since the last one */

	/* the superblock publisher, §2.2 */
	Super	pub;			/* the publishable image */
	uvlong	qidcur;

	/* procs */
	QLock	proclk;
	Rendez	procrz;
	int	nproc;
	int	stop;

	/* stages, §3.6 */
	Stage	*stages;
	ulong	nstagegrain;		/* §3.6's stagetot counter */

	/* what start-up found, §5 */
	int	bmaprebuild;
	ulong	*lost;
	ulong	nlost;
	ulong	ndirtydrop;
	uvlong	nreplay, pmax, replayhigh;
};

/*
 * A multi-request op=full stage handle, §3.6.  It is owned by its
 * creator — the /repl fid in the server — and is discarded on clunk,
 * on a Tflush of any of its chunks, on stagems of silence and at
 * restart, which is free because nothing about it is durable.
 *
 * The memory is freed only by the owner: stagediscard, or the
 * stagefinal that consumes the handle.  stagesweep *strips* an
 * expired handle — releases its reservations, zeroes its entries,
 * unlinks it and marks it dead so later calls refuse — but leaves
 * the memory, because the fid still holds the pointer and a freeing
 * sweep races every one of the owner's calls.
 */
struct Stage
{
	Store	*s;
	uchar	oid[Oidmax];
	int	oidlen;
	uvlong	len;			/* the declared final length */
	int	force;
	int	busy;			/* a chunk is in flight; qlstate */
	int	dead;			/* swept: refuse chunks and final; qlstate */
	vlong	last;			/* nsec() of the last chunk's arrival */
	ulong	*grain;			/* nblk entries, 0 = untouched */
	uchar	*dig;			/* nblk digests */
	ulong	nblk;
	ulong	ngrain;			/* grains reserved by this stage */
	Stage	*next;
};

/* alloc.c — the free-grain bitmap, the staged set and the slot spaces */
int	grainalloc(Store*, ulong*);
void	grainstageclr(Store*, ulong);
void	grainmark(Store*, ulong);
void	grainclear(Store*, ulong);
int	slotalloc(Store*, ulong*);
void	slotresvclr(Store*, ulong);
void	slotmark(Store*, ulong);
void	slotclear(Store*, ulong);
int	emapalloc(Store*, ulong*);
void	emapresvclr(Store*, ulong);
void	emapmark(Store*, ulong);
void	emapclear(Store*, ulong);
void	idxdirty(Store*, ulong slot);
void	dirtdirty(Store*, ulong slot);

/* emap.c — the extent-map cache */
Emape*	emapget(Store*, ulong slot, int fresh);
void	emapunpin(Store*, Emape*);
void	emapdirty(Store*, Emape*);
int	emapwrite(Store*, ulong slot, uchar *p);
int	emapreclaim(Store*);	/* replay only: single-proc write-back */
void	emapfreeall(Store*);

/* apply.c — §2.7's one apply function, shared by commit and replay */
int	applyrec(Store*, Objrec*, Emape*);
int	objrecok(Super*, Objrec*);	/* Super, not Store: shoalck judges too */
int	applydirty(Store*, Dirtyrec*, Dirtent**);
int	dirtyrecok(Super*, Dirtyrec*);
void	addpeer(Store*, uchar*, int);
int	applyslot(Store*, ulong slot);
void	mapopen(Store*, Omap*, Ient*, Emape*);
ulong	mapgrain(Omap*, ulong i);
uchar*	mapdig(Omap*, ulong i);
void	mapset(Omap*, ulong i, ulong grain, uchar *dig);
void	ientpack(Store*, ulong slot, uchar *p);
void	zerodigest(Store*, uvlong len, ulong i, uchar *dig);
void	ienthash(Store*, ulong slot);
void	ientunhash(Store*, ulong slot);
long	ientfind(Store*, uchar *oid, int oidlen);
void	ientinfo(Store*, ulong slot, Objinfo*);	/* caller holds qlstate */

/* commit.c — the log, group commit and the flusher */
int	logcommit(Store*, Item*);
int	flushnow(Store*);
uvlong	logused(Store*);
uvlong	logfree(Store*);

/* ckpt.c — the checkpointer and the superblock publisher */
int	checkpoint(Store*);
void	ckptproc(void*);
int	publishlocked(Store*);

/* store.c */
int	storeserving(Store*);	/* 0 and an error set on a condemned store */
int	storeproc(Store*, void (*)(void*), void*);
void	storefree(Store*);	/* the Store's memory; §13's freed hook */
void	storeprocdone(Store*);
void	storecondemn(Store*, ulong slot);	/* §5 step 10, at run time */
void	lostupdate(Store*, ulong slot);		/* /lost membership, §8 */
