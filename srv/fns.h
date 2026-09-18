/* private to srv/ */

/*
 * err.c — store.md §3.7's mapping rule, in one place (srv.h has the
 * API).  layer-a §2.6's set is declared whole, in §2.6's order, the
 * conditions no handler answers yet included: a unit that builds one
 * of those handlers finds its string here instead of adding a line to
 * this block.
 */
extern char Enoobj[];
extern char Eexists[];
extern char Edeleted[];
extern char Etoobig[];
extern char Elost[];
extern char Eunavail[];
extern char Enotready[];
extern char Ebadname[];
extern char Ereserved[];
extern char Ebadcreate[];
extern char Ebadopen[];
extern char Enorename[];
extern char Eperm[];
extern char Estaleepoch[];
extern char Efutureepoch[];
extern char Enotprimary[];
extern char Enotdisc[];
extern char Efenced[];
extern char Edown[];
extern char Edegraded[];
extern char Estalever[];
extern char Eoutofseq[];
extern char Ecsum[];
extern char Estillplaced[];
extern char Ediskfull[];
extern char Ebadctl[];
extern char Eunknownctl[];
extern char Ebadaname[];
extern char Ebadmap[];

/* not §2.6's: the two `interrupted' causes, and lib9p's own (err.c) */
extern char Einterrupted[];
extern char Edevintr[];
extern char Ebotch[];

/* text.c */
void	textread(Req*, Text*);

/* queue.c */
int	srvqinit(Srvctx*, int nq);
Qreq*	srvqprep(Srvctx*, uchar *oid, int oidlen, Req*, void (*)(Req*));
void	srvqgo(Srvctx*, Req*);
void	srvqpush(Srvctx*, uchar *oid, int oidlen, Req*, void (*)(Req*));
int	srvqjob(Srvctx*, uchar *oid, int oidlen, void (*)(void*), void*);
Qreq*	srvqprepany(Srvctx*, Req*, void (*)(Req*));
void	srvqpushany(Srvctx*, Req*, void (*)(Req*));
void	srvqflush(Req*);
Qreq*	srvqreq(Req*);
int	srvqcheck(Req*);
void	srvqhold(Req*, uvlong*);
void	srvqexit(Req*);
void	srvqwalkhold(Req*);
void	srvqanyexit(Srvctx*);
void	srvjobhold(Srvctx*);
int	srvslotfail(Srvctx*, uvlong slot);
void	srvreclaimhold(Srvctx*, uvlong i);
void	srvqdone(Req*, char *err);
void	srvqended(Qreq*);
void	srvqdrain(Srvctx*);
void	srvqfree(Srvctx*);
void	srvstep7(Req*, int onloop);
uvlong	srvpoint(Srvctx*, char*);
void	srvholdclear(Srvctx*);

/* tree.c */
void	srvfidnew(Srvctx*, Sfid*);
void	srvauxstep(Req*, int on);
void	srvfidgive(Sfid*);
void	srvfidsclose(Srvctx*);
void	srvfileqid(int file, Qid*);
void	srvobjqid(Sfid*, Objinfo*, Qid*);
void	srvdir(Srvctx*, Sfid*, Dir*);
int	srvoidok(uchar *oid, int oidlen);
void	srvwalk(Req*);
void	srvopen(Req*);
void	srvread(Req*);
void	srvwrite(Req*);
void	srvstat(Req*);
void	srvcreate(Req*);
void	srvremove(Req*);
void	srvwstat(Req*);
void	srvdestroyfid(Fid*);
void	srvdestroyreq(Req*);
void	srvopentext(Req*);

/* attach.c */
void	srvattach(Req*);
int	srvaname(Sfid*, char *aname);

/* status.c */
char*	srvstatustext(Srvctx*, Sfid*, Text*);
char*	srvmaptext(Srvctx*, Sfid*, Text*);
char*	srvemptytext(Srvctx*, Sfid*, Text*);
char*	srvdirtytext(Srvctx*, Sfid*, Text*);
char*	srvstaletext(Srvctx*, Sfid*, Text*);
char*	srvlosttext(Srvctx*, Sfid*, Text*);

/* enum.c */
Objsnap* srvsnapopen(Store*, int kinds, char *buf, int nbuf);
void	srvopenq(Req*);
void	srvobjdiropen(Req*);
void	srvobjdirread(Req*);
int	srvobjdirheld(Sfid*);
char*	srvtombstext(Srvctx*, Sfid*, Text*);
char*	srvadverttext(Srvctx*, Sfid*, Text*);

/* job.c */
char*	srvjobstext(Srvctx*, Sfid*, Text*);
char*	srvctlscrub(Srvctx*, Sfid*, int, char**);
char*	srvctlforget(Srvctx*, Sfid*, int, char**);

/* ctl.c */
void	srvctlwrite(Req*);
int	srvfencekind(Srvctx*);

/* obj.c: layer-a §2.4's object I/O, and the per-fid stage (dat.h) */
void	srvobjopen(Req*);
void	srvobjread(Req*);
void	srvobjwrite(Req*);
void	srvobjremove(Req*);
void	srvobjwstat(Req*);
void	srvobjcreate(Req*);
void	srvmetaopen(Req*);
char*	srvmetatext(Srvctx*, Sfid*, Text*);
void	srvstagesweep(Srvctx*);
void	srvstagedrain(Srvctx*);
