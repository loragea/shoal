/* private to srv/ */

/* err.c — store.md §3.7's mapping rule, in one place (srv.h has the API) */
extern char Ebadaname[];
extern char Estaleepoch[];
extern char Efutureepoch[];
extern char Eperm[];
extern char Efenced[];
extern char Ebadctl[];
extern char Eunknownctl[];
extern char Ebadname[];
extern char Enoobj[];
extern char Ecsum[];
extern char Einterrupted[];

/* text.c */
void	textread(Req*, Text*);

/* queue.c */
int	srvqinit(Srvctx*, int nq);
Qreq*	srvqprep(Srvctx*, uchar *oid, int oidlen, Req*, void (*)(Req*));
void	srvqgo(Srvctx*, Req*);
void	srvqpush(Srvctx*, uchar *oid, int oidlen, Req*, void (*)(Req*));
void	srvqflush(Req*);
Qreq*	srvqreq(Req*);
int	srvqcheck(Req*);
void	srvqdone(Req*, char *err);
void	srvqended(Qreq*);
void	srvqdrain(Srvctx*);
void	srvqfree(Srvctx*);
void	srvstep7(Srvctx*, uchar *oid, int oidlen);

/* tree.c */
void	srvattachqid(Sfid*, Qid*);
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

/* attach.c */
void	srvattach(Req*);
int	srvaname(Sfid*, char *aname);

/* status.c */
char*	srvstatustext(Srvctx*, Sfid*, Text*);
char*	srvmaptext(Srvctx*, Sfid*, Text*);
char*	srvemptytext(Srvctx*, Sfid*, Text*);

/* ctl.c */
void	srvctlwrite(Req*);
int	srvfencekind(Srvctx*);
