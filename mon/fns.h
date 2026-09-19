/* private to mon/ */

/*
 * err.c: layer-a §2.6's set, of which six are the monitor's own to
 * emit.  The rest are here for the reason err.c gives — a unit that
 * builds the handler for one of them names a string that is already
 * written down.
 */
extern char Emnoobj[];
extern char Emexists[];
extern char Emdeleted[];
extern char Emtoobig[];
extern char Emlost[];
extern char Emunavail[];
extern char Emnotready[];
extern char Embadname[];
extern char Emreserved[];
extern char Embadcreate[];
extern char Embadopen[];
extern char Emnorename[];
extern char Emperm[];
extern char Emstaleepoch[];
extern char Emfutureepoch[];
extern char Emnotprimary[];
extern char Emnotdisc[];
extern char Emfenced[];
extern char Emdown[];
extern char Emdegraded[];
extern char Emstalever[];
extern char Emoutofseq[];
extern char Emcsum[];
extern char Emstillplaced[];
extern char Emdiskfull[];
extern char Embadctl[];
extern char Emunknownctl[];
extern char Embadaname[];
extern char Embadmap[];

/* text.c */
void	montextread(Req*, Mtext*);

/* attach.c */
int	monaname(Mfid*, char*);
void	monsrvattach(Req*);

/* tree.c */
void	monsrvwalk(Req*);
void	monsrvopen(Req*);
void	monsrvread(Req*);
void	monsrvwrite(Req*);
void	monsrvstat(Req*);
void	monsrvcreate(Req*);
void	monsrvremove(Req*);
void	monsrvwstat(Req*);
void	monsrvdestroyfid(Fid*);
void	monfileqid(int file, Qid*);
void	mondir(Monctx*, Mfid*, Dir*);

/* status.c */
char*	monmaptext(Monctx*, Mfid*, Mtext*);
char*	monmapfiletext(Monctx*, Mfid*, Mtext*);
char*	moninstancestext(Monctx*, Mfid*, Mtext*);
char*	monstaletext(Monctx*, Mfid*, Mtext*);
char*	monhealthtext(Monctx*, Mfid*, Mtext*);
char*	monstatustext(Monctx*, Mfid*, Mtext*);
char*	monemptytext(Monctx*, Mfid*, Mtext*);

/* ctl.c */
void	monctlwrite(Req*);

/* mon.c */
void	monsrvseen(Monctx*, char *iid);
