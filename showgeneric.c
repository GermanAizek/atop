/*
** ATOP - System & Process Monitor 
**
** The program 'atop' offers the possibility to view the activity of
** the system on system-level as well as process-level.
** 
** This source-file contains the print-functions to visualize the calculated
** figures.
** ==========================================================================
** Author:      Gerlof Langeveld
** E-mail:      gerlof.langeveld@atoptool.nl
** Date:        November 1996
** LINUX-port:  June 2000
** --------------------------------------------------------------------------
** Copyright (C) 2000-2010 Gerlof Langeveld
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of the GNU General Public License as published by the
** Free Software Foundation; either version 2, or (at your option) any
** later version.
**
** This program is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
** See the GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
** --------------------------------------------------------------------------
*/
#define _POSIX_C_SOURCE
#define _XOPEN_SOURCE
#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <curses.h>
#include <pwd.h>
#include <grp.h>
#include <regex.h>
#include <locale.h>
#include <unistd.h>
#include <sys/select.h>

#include "atop.h"
#include "photoproc.h"
#include "photosyst.h"
#include "cgroups.h"
#include "showgeneric.h"
#include "showlinux.h"

static struct pselection procsel = {"", {USERSTUB, }, {0,},
                                    "", 0, { 0, },  "", 0, { 0, }, "", "" };
static struct sselection syssel;

static void	showhelp(int);
static int	fixedhead;	/* boolean: fixate header-lines         */
static int	sysnosort;	/* boolean: suppress sort of resources  */
static int	threadsort;	/* boolean: sort threads per process    */
static int	avgval;		/* boolean: average values i.s.o. total */
static int	suppressexit;	/* boolean: suppress terminated processes */

static char	showtype  = MPROCGEN;
static char	showorder = MSORTCPU;

static int	maxcpulines = 999;  /* maximum cpu       lines          */
static int	maxgpulines = 999;  /* maximum gpu       lines          */
static int	maxdsklines = 999;  /* maximum disk      lines          */
static int	maxmddlines = 999;  /* maximum MDD       lines          */
static int	maxlvmlines = 999;  /* maximum LVM       lines          */
static int	maxintlines = 999;  /* maximum interface lines          */
static int	maxifblines = 999;  /* maximum infinibnd lines          */
static int	maxnfslines = 999;  /* maximum nfs mount lines          */
static int	maxcontlines = 999; /* maximum container lines          */
static int	maxnumalines = 999; /* maximum numa      lines          */
static int	maxllclines = 999;  /* maximum llc       lines          */

static short	colorinfo   = COLOR_GREEN;
static short	coloralmost = COLOR_CYAN;
static short	colorcrit   = COLOR_RED;
static short	colorthread = COLOR_YELLOW;

static char	winchange;
static char	keywaiting;	// set after key has been pushed back

int		paused;    		// boolean: currently in pause-mode
int		cgroupdepth = 7;	// default: cgroups without processes

static int	cumusers(struct tstat **, struct tstat *, int);
static int	cumprogs(struct tstat **, struct tstat *, int);
static int	cumconts(struct tstat **, struct tstat *, int);
static void	accumulate(struct tstat *, struct tstat *);

static int	procsuppress(struct tstat *, struct pselection *);
static void	limitedlines(void);
static long	getnumval(char *, long, int);
static void	getsigwinch(int);
static void	generic_init(void);
static char	text_samp(time_t, int, struct devtstat *, struct sstat *,
	   		struct cgchainer *, int, int, unsigned int, char);

static int	(*procsort[])(const void *, const void *) = {
			[MSORTCPU&0x1f]=compcpu, 
			[MSORTMEM&0x1f]=compmem, 
			[MSORTDSK&0x1f]=compdsk, 
			[MSORTNET&0x1f]=compnet, 
			[MSORTGPU&0x1f]=compgpu, 
};

extern detail_printpair ownprocs[];

/*
** global: incremented by -> key and decremented by <- key
*/
int	startoffset;

/*
** main function to handle sample generically
** to show flat text, full screen text or bar graphs
*/
char
generic_samp(time_t curtime, int nsecs,
           struct devtstat *devtstat, struct sstat *sstat,
	   struct cgchainer *cstats, int ncgroups, int npids,
           int nexit, unsigned int noverflow, char flag)
{
	static char	firstcall = 1;
	char		retval, sorted = 0;

	if (firstcall)
	{
		generic_init();
		firstcall = 0;
	}

	while (1)
	{
		if (displaymode == 'T')		// text mode?
		{
			// show sample and wait for input or timer expiration
			//
			switch (retval = text_samp(curtime, nsecs,
				devtstat, sstat, cstats, ncgroups,
				nexit, noverflow, flag))
			{
			   case MBARGRAPH:
				displaymode = 'D';
				break;

  			   default:
				return retval;
			}

			sorted = 1;	// resources have been sorted by text mode
		}

		if (displaymode == 'D')		// draw mode?
		{
			// show sample and wait for input or timer expiration
			//
			switch (retval = draw_samp(curtime, nsecs, sstat, flag, sorted))
			{
			   case MBARGRAPH:	// just switch to text mode
				displaymode = 'T';
				break;

			   case MCGROUPS:	// switch to text mode: cgroups
				if (supportflags & CGROUPV2)
					showtype  = MCGROUPS;
				else
					showtype  = MPROCGEN;


				displaymode = 'T';
				break;

			   case MPROCGEN:	// switch to text mode: generic
				showtype  = MPROCGEN;

				if (showorder != MSORTAUTO)
					showorder = MSORTCPU;

				displaymode = 'T';
				break;

 			   case MPROCMEM:	// switch to text mode: memory
				showtype  = MPROCMEM;

				if (showorder != MSORTAUTO)
					showorder = MSORTMEM;

				displaymode = 'T';
				break;

			   case MPROCDSK:	// switch to text mode: disk
				showtype  = MPROCDSK;

				if (showorder != MSORTAUTO)
					showorder = MSORTDSK;

				displaymode = 'T';
				break;

			   case MPROCNET:	// switch to text mode: network
				if (supportflags & NETATOP || supportflags & NETATOPBPF)
				{
					showtype  = MPROCNET;

					if (showorder != MSORTAUTO)
						showorder = MSORTNET;
				}
				else
				{
					showtype  = MPROCGEN;
					showorder = MSORTCPU;
				}

				displaymode = 'T';
				break;

			   case MPROCGPU:	// switch to text mode: GPU
				if (supportflags & GPUSTAT)
				{
					showtype  = MPROCGPU;

					if (showorder != MSORTAUTO)
						showorder = MSORTGPU;
				}
				else
				{
					showtype  = MPROCGEN;
					showorder = MSORTCPU;
				}

				displaymode = 'T';
				break;

			   case MPROCSCH:	// switch to text mode: scheduling
				showtype  = MPROCSCH;

				if (showorder != MSORTAUTO)
					showorder = MSORTCPU;

				displaymode = 'T';
				break;

			   case MPROCVAR:	// switch to text mode: various
				showtype  = MPROCVAR;

				displaymode = 'T';
				break;

			   case MPROCARG:	// switch to text mode: arguments
				showtype  = MPROCARG;

				displaymode = 'T';
				break;

  			   default:
				return retval;
			}
		}
	}
}

/*
** print the deviation-counters on process level, cgroups level and
** system level in text mode
*/
static char
text_samp(time_t curtime, int nsecs,
           struct devtstat *devtstat, struct sstat *sstat, 
	   struct cgchainer *cgchainers, int ncgroups,
           int nexit, unsigned int noverflow, char flag)
{
	register int	i, curline, statline, nproc;
	int		firstitem=0, slistsz, alistsz, killpid, killsig;
	int		lastchar;
	char		format1[16], format2[16], branchtime[32];
	char		*statmsg = NULL, statbuf[80], genline[80];
	char		 *lastsortp, curorder, autoorder;
	char		buf[33];
	struct passwd 	*pwd;
	struct syscap	syscap;

	fd_set		readfds;
	char		eventbuf[1024];
	int		nrfds;

	/*
	** number of entries in the active list of cgroups/tasks
	** to be displayed
	*/
	int ncurlist = 0;

	/*
	** cgroupsel points to the merged list of cgchainer pointers (to cgroups)
	** and tstat pointers (to processes), representing the cgroups information
	** to be displayed (for cgroups visualization);
	** ncurlist indicates the number of entries in this list
	** 
	** this list will only be allocated 'lazy' only when
	** cgroups visualization is requested
	**
	** cgroupsort refers to a list with cgchainer pointers in
	** sorted order according to the current showorder
	*/
	struct cglinesel *cgroupsel   = 0;
	struct cgchainer **cgroupsort = 0;
	char             cstatdeviate = ' ', cstatdepth = ' ', cstatorder = ' ';

	/*
	** curlist points to the active list of tstat pointers that
	** should be displayed; ncurlist indicates the number of entries in
	** this list
	*/
	struct tstat	**curlist;

	/*
	** tXcumlist is a list of tstat structs holding one entry
	** per accumulated (per user or per program) group of processes
	**
	** Xcumlist contains the pointers to all structs in tXcumlist
	** 
	** these lists will only be allocated 'lazy'
	** only when accumulation is requested
	*/
	struct tstat	*tpcumlist = 0;		// per program accumulation
	struct tstat	**pcumlist = 0;
	int		npcum      = 0;
	char		plastorder = 0;

	struct tstat	*tucumlist = 0;		// per user accumulation
	struct tstat	**ucumlist = 0;
	int		nucum      = 0;
	char		ulastorder = 0;

	struct tstat	*tccumlist = 0;		// per container/pod accumulation
	struct tstat	**ccumlist = 0;
	int		nccum      = 0;
	char		clastorder = 0;

	/*
	** tsklist contains the pointers to all structs in tstat
	** sorted on process with the related threads immediately
	** following the process
	**
	** this list will be allocated 'lazy'
	*/
	struct tstat	**tsklist  = 0;
	int		ntsk       = 0;
	char		tlastorder = 0;
	char		zipagain   = 0;
	char		tdeviate   = 0;

	/*
	** sellist contains the pointers to the structs in tstat
	** that are currently selected on basis of a particular
	** username (regexp), program name (regexp), container/pod name
	** or suppressed terminated procs
	**
	** this list will be allocated 'lazy'
	*/
	struct tstat	**sellist  = 0;
	int		nsel       = 0;
	char		slastorder = 0;
	char		threadallowed = 0;

	startoffset = 0;

	/*
	** compute the total capacity of this system for the 
	** four main resources
	*/
	totalcap(&syscap, sstat, devtstat->procactive, devtstat->nprocactive);

	/*
	** sort per-cpu       		statistics on busy percentage
	** sort per-logical-volume      statistics on busy percentage
	** sort per-multiple-device     statistics on busy percentage
	** sort per-disk      		statistics on busy percentage
	** sort per-interface 		statistics on busy percentage (if known)
	*/
	if (!sysnosort)
	{
		if (sstat->cpu.nrcpu > 1 && maxcpulines > 0)
			qsort(sstat->cpu.cpu, sstat->cpu.nrcpu,
	 	               sizeof sstat->cpu.cpu[0], cpucompar);

		if (sstat->gpu.nrgpus > 1 && maxgpulines > 0)
			qsort(sstat->gpu.gpu, sstat->gpu.nrgpus,
	 	               sizeof sstat->gpu.gpu[0], gpucompar);

		if (sstat->dsk.nlvm > 1 && maxlvmlines > 0)
			qsort(sstat->dsk.lvm, sstat->dsk.nlvm,
			       sizeof sstat->dsk.lvm[0], diskcompar);

		if (sstat->dsk.nmdd > 1 && maxmddlines > 0)
			qsort(sstat->dsk.mdd, sstat->dsk.nmdd,
			       sizeof sstat->dsk.mdd[0], diskcompar);

		if (sstat->dsk.ndsk > 1 && maxdsklines > 0)
			qsort(sstat->dsk.dsk, sstat->dsk.ndsk,
			       sizeof sstat->dsk.dsk[0], diskcompar);

		if (sstat->intf.nrintf > 1 && maxintlines > 0)
			qsort(sstat->intf.intf, sstat->intf.nrintf,
		  	       sizeof sstat->intf.intf[0], intfcompar);

		if (sstat->ifb.nrports > 1 && maxifblines > 0)
			qsort(sstat->ifb.ifb, sstat->ifb.nrports,
		  	       sizeof sstat->ifb.ifb[0], ifbcompar);

		if (sstat->nfs.nfsmounts.nrmounts > 1 && maxnfslines > 0)
			qsort(sstat->nfs.nfsmounts.nfsmnt,
		              sstat->nfs.nfsmounts.nrmounts,
		  	      sizeof sstat->nfs.nfsmounts.nfsmnt[0],
				nfsmcompar);

		if (sstat->cfs.nrcontainer > 1 && maxcontlines > 0)
			qsort(sstat->cfs.cont, sstat->cfs.nrcontainer,
		  	       sizeof sstat->cfs.cont[0], contcompar);

		if (sstat->memnuma.nrnuma > 1 && maxnumalines > 0)
			qsort(sstat->memnuma.numa, sstat->memnuma.nrnuma,
			       sizeof sstat->memnuma.numa[0], memnumacompar);

		if (sstat->cpunuma.nrnuma > 1 && maxnumalines > 0)
			qsort(sstat->cpunuma.numa, sstat->cpunuma.nrnuma,
			       sizeof sstat->cpunuma.numa[0], cpunumacompar);

		if (sstat->llc.nrllcs > 1 && maxllclines > 0)
			qsort(sstat->llc.perllc, sstat->llc.nrllcs,
				sizeof sstat->llc.perllc[0], llccompar);
	}

	/*
	** loop in which the system resources and the list of active
	** processes are shown; the loop will be preempted by receiving
	** a timer-signal or when the trigger-button is pressed.
	*/
	while (1)
	{
		curline = 1;
		genline[0] = '\0';

	        /*
       	 	** prepare screen or file output for new sample
        	*/
        	if (screen)
               	 	werase(stdscr);
        	else
                	printf("\n\n");

        	/*
        	** print general headerlines
        	*/
        	convdate(curtime, format1);       /* date to ascii string   */
        	convtime(curtime, format2);       /* time to ascii string   */

		if (screen)
			attron(A_REVERSE);

                int seclen	= val2elapstr(nsecs, buf);
                int lenavail 	= (screen ? COLS : linelen) -
						60 - seclen - utsnodenamelen;
                int len1	= lenavail / 3;

		if (len1 <= 0)
			len1 = 1;

                int len2	= lenavail - len1 - len1; 

		printg("ATOP - %s%*s%s %s %s %*s"
		       "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%*s%s elapsed", 
			utsname.nodename, len1, "", 
			format1, format2, "      ", len1, "",
			threadview                    ? MTHREAD    : '-',
			threadsort                    ? MTHRSORT   : '-',
			fixedhead  		      ? MSYSFIXED  : '-',
			sysnosort  		      ? MSYSNOSORT : '-',
			deviatonly 		      ? '-'        : MALLACTIVE,
			usecolors  		      ? '-'        : MCOLORS,
			avgval     		      ? MAVGVAL    : '-',
			calcpss     		      ? MCALCPSS   : '-',
			getwchan     		      ? MGETWCHAN  : '-',
			suppressexit 		      ? MSUPEXITS  : '-',
			procsel.userid[0] != USERSTUB ? MSELUSER   : '-',
			procsel.prognamesz	      ? MSELPROC   : '-',
			procsel.utsname[0]	      ? MSELCONT   : '-',
			procsel.pid[0] != 0	      ? MSELPID    : '-',
			procsel.argnamesz	      ? MSELARG    : '-',
			procsel.states[0]	      ? MSELSTATE  : '-',
			syssel.lvmnamesz +
			syssel.dsknamesz +
			syssel.itfnamesz	      ? MSELSYS    : '-',
			cgroupdepth+0x30,
			len2, "", buf);

		if (screen)
			attroff(A_REVERSE);
                else
                        printg("\n");

		if (screen && paused)
		{
			if (usecolors)
				attron(COLOR_PAIR(FGCOLORBORDER));

			attron(A_REVERSE);
			mvprintw(0, 27 + utsnodenamelen + len1, "PAUSED");
			attroff(A_REVERSE);

			if (usecolors)
				attroff(COLOR_PAIR(FGCOLORBORDER));
		}

		/*
		** print cumulative system- and user-time for all processes
		*/
		pricumproc(sstat, devtstat, nexit, noverflow, avgval, nsecs);

		if (noverflow)
		{
			snprintf(statbuf, sizeof statbuf, 
			         "Only %d terminated processes handled "
			         "-- %u skipped!", nexit, noverflow);

			statmsg = statbuf;
		}

		curline=2;

		/*
		** print other lines of system-wide statistics
		*/
		if (showorder == MSORTAUTO)
			autoorder = MSORTCPU;
		else
			autoorder = showorder;

		curline = prisyst(sstat, curline, nsecs, avgval,
		                  fixedhead, &syssel, &autoorder,
		                  maxcpulines, maxgpulines, maxdsklines,
				  maxmddlines, maxlvmlines,
		                  maxintlines, maxifblines, maxnfslines,
		                  maxcontlines, maxnumalines, maxllclines);

		/*
 		** if system-wide statistics do not fit,
		** limit the number of variable resource lines
		** and try again
		*/
		if (screen && curline+2 > LINES)
		{
			curline = 2;

			move(curline, 0);
			clrtobot();
			move(curline, 0);

			limitedlines();
			
			curline = prisyst(sstat, curline, nsecs, avgval,
					fixedhead,  &syssel, &autoorder,
					maxcpulines, maxgpulines,
					maxdsklines, maxmddlines,
		                        maxlvmlines, maxintlines,
					maxifblines, maxnfslines,
		                        maxcontlines, maxnumalines,
					maxllclines);

			/*
 			** if system-wide statistics still do not fit,
			** the window is really to small
			*/
			if (curline+2 > LINES)
			{
				endwin();	// finish curses interface

				fprintf(stderr,
				      "Not enough screen-lines available "
				      "(need at least %d lines)\n", curline+2);
				fprintf(stderr, "Please resize window....\n");

				cleanstop(1);
			}
			else
			{
				statmsg = "Number of variable resources"
				          " limited to fit in this window";
			}
		}

		statline = curline;

		if (screen)
        	        move(curline, 0);

		if (statmsg)
		{
			if (screen)
			{
				clrtoeol();
				if (usecolors)
					attron(COLOR_PAIR(FGCOLORINFO));
			}

			printg(statmsg);

			if (screen)
			{
				if (usecolors)
					attroff(COLOR_PAIR(FGCOLORINFO));
			}

			statmsg = NULL;
		}
		else
		{
			if (flag&RRBOOT)
			{
				char *initmsg = "*** System and Process Activity since Boot ***";
				char *viewmsg;

				if (rawreadflag)
				{
					if (twinpid)
						viewmsg   = "Twin mode";
					else
						viewmsg   = "Rawfile view";
				}
				else
				{
					if (rootprivs())
						viewmsg   = "Unrestricted view (privileged)";
					else
						viewmsg   = "Restricted view (unprivileged)";
				}

				if (screen)
				{
					if (usecolors)
						attron(COLOR_PAIR(FGCOLORINFO));
				}

       				printg(initmsg);

				if (screen)
				{
					if (usecolors)
						attroff(COLOR_PAIR(FGCOLORINFO));

					printg("%*s", COLS - strlen(initmsg)
					                   - strlen(viewmsg),
					                     " ");
				}
				else
				{
					printg("%*s", 80 - strlen(initmsg)
					                 - strlen(viewmsg),
					                   " ");
				}

				if (screen)
				{
					if (usecolors)
						attron(COLOR_PAIR(FGCOLORALMOST));
				}

       				printg(viewmsg);

				if (screen)
				{
					if (usecolors)
						attroff(COLOR_PAIR(FGCOLORALMOST));
				}

			}
		}

		if (showtype != MCGROUPS)
		{
			/*
			** select the required list with tasks to be shown
			**
			** if cumulative figures required, accumulate resource
			** consumption of all processes in the current list
			*/
			switch (showtype)
			{
			   case MCUMUSER:
				threadallowed = 0;
	
				if (ucumlist)	/* previous list still available? */
				{
					free(ucumlist);
					free(tucumlist);
					ulastorder = 0;
				}
	
				if (deviatonly)
					nproc = devtstat->nprocactive;
				else
					nproc = devtstat->nprocall;
	
				/*
				** allocate space for new (temporary) list with
				** one entry per user (list has worst-case size)
				*/
				tucumlist = calloc(sizeof(struct tstat),    nproc);
				ucumlist  = malloc(sizeof(struct tstat *) * nproc);
	
				ptrverify(tucumlist,
				        "Malloc failed for %d ucum procs\n", nproc);
				ptrverify(ucumlist,
				        "Malloc failed for %d ucum ptrs\n",  nproc);
	
				for (i=0; i < nproc; i++)
				{
					/* fill pointers */
					ucumlist[i] = tucumlist+i;
				}
	
				nucum = cumusers(deviatonly ?
							devtstat->procactive :
							devtstat->procall,
							tucumlist, nproc);
	
				curlist   = ucumlist;
				ncurlist  = nucum;
				lastsortp = &ulastorder;
				break;
	
			   case MCUMPROC:
				threadallowed = 0;
	
				if (pcumlist)	/* previous list still available? */
				{
					free(pcumlist);
					free(tpcumlist);
					plastorder = 0;
				}
	
				if (deviatonly)
					nproc = devtstat->nprocactive;
				else
					nproc = devtstat->nprocall;
	
				/*
				** allocate space for new (temporary) list with
				** one entry per program (list has worst-case size)
				*/
				tpcumlist = calloc(sizeof(struct tstat),    nproc);
				pcumlist  = malloc(sizeof(struct tstat *) * nproc);
	
				ptrverify(tpcumlist,
				        "Malloc failed for %d pcum procs\n", nproc);
				ptrverify(pcumlist,
				        "Malloc failed for %d pcum ptrs\n",  nproc);
	
				for (i=0; i < nproc; i++)
				{	
					/* fill pointers */
					pcumlist[i] = tpcumlist+i;
				}
	
				npcum = cumprogs(deviatonly ?
							devtstat->procactive :
							devtstat->procall,
							tpcumlist, nproc);
	
				curlist   = pcumlist;
				ncurlist  = npcum;
				lastsortp = &plastorder;
				break;
	
			   case MCUMCONT:
				threadallowed = 0;
	
				if (ccumlist)	/* previous list still available? */
				{
					free(ccumlist);
					free(tccumlist);
					clastorder = 0;
				}
	
				if (deviatonly)
					nproc = devtstat->nprocactive;
				else
					nproc = devtstat->nprocall;
	
				/*
				** allocate space for new (temporary) list with
				** one entry per user (list has worst-case size)
				*/
				tccumlist = calloc(sizeof(struct tstat),    nproc);
				ccumlist  = malloc(sizeof(struct tstat *) * nproc);
	
				ptrverify(tccumlist,
				        "Malloc failed for %d ccum procs\n", nproc);
				ptrverify(ccumlist,
				        "Malloc failed for %d ccum ptrs\n",  nproc);
	
				for (i=0; i < nproc; i++)
				{
					/* fill pointers */
					ccumlist[i] = tccumlist+i;
				}
	
				nccum = cumconts(deviatonly ?
							devtstat->procactive :
							devtstat->procall,
							tccumlist, nproc);
	
				curlist   = ccumlist;
				ncurlist  = nccum;
				lastsortp = &clastorder;
				break;
	
			   default:
				threadallowed = 1;
	
				if (deviatonly && showtype  != MPROCMEM &&
				                  showorder != MSORTMEM   )
				{
					curlist   = devtstat->procactive;
					ncurlist  = devtstat->nprocactive;
				}
				else
				{
					curlist   = devtstat->procall;
					ncurlist  = devtstat->nprocall;
				}
	
				lastsortp = &tlastorder;
	
				if ( procsel.userid[0] == USERSTUB &&
				    !procsel.prognamesz            &&
				    !procsel.utsname[0]            &&
				    !procsel.states[0]             &&
				    !procsel.argnamesz             &&
				    !procsel.pid[0]                &&
				    !suppressexit                    )
					/* no selection wanted */
					break;
	
				/*
				** selection specified for tasks:
				** create new (worst case) pointer list if needed
				*/
				free(sellist); // remove previous list if needed
	
				sellist = malloc(sizeof(struct tstat *) * ncurlist);
	
				ptrverify(sellist,
				       "Malloc failed for %d select ptrs\n", ncurlist);
	
				for (i=nsel=0; i < ncurlist; i++)
				{
					if (procsuppress(*(curlist+i), &procsel))
						continue;
	
					if (curlist[i]->gen.state == 'E' &&
					    suppressexit                   )
						continue;
	
					sellist[nsel++] = curlist[i]; 
				}
	
				curlist    = sellist;
				ncurlist   = nsel;
				tlastorder = 0; /* new sort and zip normal view */
				slastorder = 0;	/* new sort and zip now         */
				lastsortp  = &slastorder;
			}
	
			/*
			** sort the list in required order 
			** (default CPU-consumption) and print the list
			*/
			if (showorder == MSORTAUTO)
				curorder = autoorder;
			else
				curorder = showorder;
	
			/*
 			** determine size of list to be displayed
			*/
			if (screen)
				slistsz = LINES-curline-2;
			else
				if (threadview && threadallowed)
					slistsz = devtstat->ntaskactive;
				else
					slistsz = ncurlist;

			if (ncurlist > 0 && slistsz > 0)
			{
				/*
 				** if sorting order is changed, sort again
 				*/
				if (*lastsortp != curorder)
				{
					qsort(curlist, ncurlist,
					        sizeof(struct tstat *),
					        procsort[(int)curorder&0x1f]);
	
					*lastsortp = curorder;
	
					zipagain = 1;
				}
	
				if (threadview && threadallowed)
				{
					int ntotal, j, t;
	
					if (deviatonly && showtype  != MPROCMEM &&
				      	                  showorder != MSORTMEM   )
						ntotal = devtstat->ntaskactive;
					else
						ntotal = devtstat->ntaskall;
	
					/*
  					** check if existing pointer list still usable
					** if not, allocate new pointer list to be able
					** to zip process list with references to threads
					*/
					if (!tsklist || ntsk != ntotal ||
								tdeviate != deviatonly)
					{
						free(tsklist);	// remove current
	
						tsklist = malloc(sizeof(struct tstat *)
									    * ntotal);
	
						ptrverify(tsklist,
					             "Malloc failed for %d taskptrs\n",
					             ntotal);
	
						ntsk     = ntotal;
						tdeviate = deviatonly;
	
						zipagain = 1;
					}
					else
						j = ntotal;
	
					/*
 					** zip process list with thread list
					*/
					if (zipagain)
					{
						struct tstat *tall = devtstat->taskall;
						struct tstat *pcur;
						long int     n;
	
						for (i=j=0; i < ncurlist; i++)
						{
						    pcur = curlist[i];
	
						    tsklist[j++] = pcur; // take process
	
						    n = j; // start index of threads
	
						    for (t = pcur - tall + 1;
						         t < devtstat->ntaskall &&
							 pcur->gen.tgid		&&
						         pcur->gen.tgid == 
						            (tall+t)->gen.tgid;
						         t++)
						    {
							if (procsuppress(tall+t, &procsel))
								continue;

							if (deviatonly &&
								showtype  != MPROCMEM &&
							        showorder != MSORTMEM   )
							{
							   if (!(tall+t)->gen.wasinactive)
								tsklist[j++] = tall+t;
 							}
							else
							{
								tsklist[j++] = tall+t;
							}
						    }
	
					            if (threadsort && j-n > 0 &&
								curorder != MSORTMEM)
						    {
							qsort(&tsklist[n], j-n,
					                  sizeof(struct tstat *),
					                  procsort[(int)curorder&0x1f]);
						    }
						}
	
						zipagain = 0;
					}
	
					curlist  = tsklist;
					ncurlist = j;
				}

				/*
				** print the header
				** first determine the column-header for the current
				** sorting order of processes
				*/
				if (screen)
				{
					attron(A_REVERSE);
       		                         move(curline+1, 0);
       		                }
	
				prihead(firstitem/slistsz+1, (ncurlist-1)/slistsz+1,
			       		&showtype, &curorder,
					showorder == MSORTAUTO ? 1 : 0,
					sstat->cpu.nrcpu);

				if (screen)
				{
					attroff(A_REVERSE);
					clrtobot();
				}

				/*
				** print the list
				*/
				priproc(curlist, firstitem, ncurlist, curline+2,
				        firstitem/slistsz+1, (ncurlist-1)/slistsz+1,
			        	showtype, curorder, &syscap, nsecs, avgval);
			}
		}
		else	// MCGROUPS: print cgroups
		{
			/*
			** sort the list in required order 
			** (default CPU-consumption) and print the list
			*/
			if (showorder == MSORTAUTO)
				curorder = autoorder;
			else
				curorder = showorder;

			/*
			** make new list with a selection (if needed) of cgroups
			** merged with processes related to those cgroups
			*/
			if (cgroupsel == NULL           ||	// not created yet
			    cstatdeviate != deviatonly  ||	// or not the right contents?
			    cstatdepth   != cgroupdepth ||
			    cstatorder   != curorder      )
			{
				struct tstat **tp;

				/*
				** get sorted list of pointers to the cgchainer structs
				**
				** when a list has been created already that is
				** not suitable, first remove it
				*/
				free(cgroupsort);

				cgroupsort = cgsort(cgchainers, ncgroups, curorder);

				/*
				** determine required list of processes (all or active)
				** for memory usage even non-active processes are taken
				*/
				if (deviatonly && curorder != MSORTMEM)
				{
					nproc = devtstat->nprocactive;
					tp    = devtstat->procactive;
				}
				else
				{
					nproc = devtstat->nprocall;
					tp    = devtstat->procall;
				}

				/*
				** when a selection list has been created
				** already that is not suitable, first remove it
				*/
				free(cgroupsel);

				/*
				** create new merged list of cgroups and processes
				*/
				ncurlist = mergecgrouplist(&cgroupsel, cgroupdepth,
						cgroupsort, ncgroups, tp, nproc, curorder);

				/*
				** preserve list characteristics
				*/
				cstatdeviate = deviatonly;
				cstatdepth   = cgroupdepth;
				cstatorder   = curorder;
			}

			/*
 			** determine size of list to be displayed
			*/
			if (screen)
				slistsz = LINES-curline-2;
			else
				slistsz = ncurlist;

			if (ncurlist > 0 && slistsz > 0)
			{
				/*
				** print the header
				*/
				if (screen)
				{
					attron(A_REVERSE);
					move(curline+1, 0);
       		        	}

				prihead(firstitem/slistsz+1, (ncurlist-1)/slistsz+1,
			      		&showtype, &curorder,
					curorder == MSORTAUTO ? 1 : 0,
					sstat->cpu.nrcpu);

				if (screen)
				{
					attroff(A_REVERSE);
					clrtobot();
				}

				/*
				** print the list of cgroups
				*/
				pricgroup(cgroupsel, firstitem, ncurlist, curline+2,
			        	firstitem/slistsz+1, (ncurlist-1)/slistsz+1,
					&syscap, nsecs, avgval);
			}
		}

		alistsz = ncurlist;	/* preserve size of active list */

		/*
		** in case of writing to a terminal, the user can also enter
		** a character to switch options, etc
		*/
		if (screen)
		{
			/*
			** refresh screen output generated sofar
			*/
			move(statline, 0);

			refresh();

			if (twinpid && !keywaiting)	// twin mode?
			{
				struct sigaction sigact, sigold;

				/*
				** catch window size changes while in select
				*/
			        memset(&sigact, 0, sizeof sigact);
        			sigact.sa_handler = getsigwinch;
        			sigaction(SIGWINCH, &sigact, &sigold);

				winchange = 0;

				/*
				** await input character from keyboard, or
				**       inotify trigger in case of twin mode, or
				**       interval timer expiration
				*/
				FD_ZERO(&readfds);
				FD_SET(0, &readfds);

				if (!paused && fdinotify != -1)	// twin mode?
				{
					FD_SET(fdinotify, &readfds);
					nrfds = fdinotify + 1;
				}
				else
				{
					nrfds = 1;
				}

				switch (select(nrfds, &readfds, (fd_set *)0,
			                      (fd_set *)0, (struct timeval *)0))
				{
				   case -1:
					/*
					** window change or timer expiration?
					*/
					if (winchange)
					{
						// window change: set new dimensions
						struct winsize w;

						ioctl(0, TIOCGWINSZ, &w);
						resizeterm(w.ws_row, w.ws_col);
						lastchar = KEY_RESIZE;
					}
					else
					{
						// time interrupt
						lastchar = 0;
					}
					break;

				   default:
					/*
					** inotify trigger that new sample has been written?
					** pretend as if the 't' key has been pressed
					** to read that sample
					**
					** otherwise: read keystroke from keyboard
					*/
					if (FD_ISSET(fdinotify, &readfds))
					{
						read(fdinotify, eventbuf, sizeof eventbuf);
	
						lastchar = MSAMPNEXT;
					}
					else
					{
						lastchar = getch();
					}
				}

        			sigaction(SIGWINCH, &sigold, (struct sigaction *)0);
			}
			else	// no twin mode: neutral state is getch()
			{
				lastchar = getch();
				keywaiting = 0;
			}

			switch (lastchar)
			{
			   /*
			   ** timer expired
			   */
			   case ERR:
			   case 0:
				timeout(0);
				(void) getch();
				timeout(-1);

				goto free_and_return;	

			   /*
			   ** stop it
			   */
			   case MQUIT:
				move(LINES-1, 0);
				clrtoeol();
				refresh();
				cleanstop(0);

			   /*
			   ** switch to bar graph mode
			   */
			   case MBARGRAPH:
				erase();
				refresh();

				goto free_and_return;	

			   /*
			   ** manual trigger for next sample
			   */
			   case MSAMPNEXT:
				if (paused && !twinpid)
				{
					beep();
					break;
				}

				getalarm(0);

				goto free_and_return;	

			   /*
			   ** manual trigger for previous sample
			   */
			   case MSAMPPREV:
				if (!rawreadflag)
				{
					statmsg = "Only allowed in twin mode or "
						  "when viewing raw file!";
					beep();
					break;
				}

				if (!paused && twinpid)
				{
					paused=1;	// implicit pause in twin mode
					clrtoeol();
					refresh();
				}

				goto free_and_return;	

                           /*
			   ** branch to certain time stamp
                           */
                           case MSAMPBRANCH:
                                if (!rawreadflag)
                                {
					statmsg = "Only allowed in twin mode or "
						  "when viewing raw file!";
                                        beep();
                                        break;
                                }

				if (!paused && twinpid)
				{
					paused=1;	// implicit pause in twin mode
					clrtoeol();
					refresh();
				}

                                echo();
                                move(statline, 0);
                                clrtoeol();
                                printw("Enter new time "
				       "(format [YYYYMMDD]hhmm[ss]): ");

                                branchtime[0] = '\0';
                                scanw("%31s\n", branchtime);
                                noecho();

				begintime = cursortime;

                                if ( !getbranchtime(branchtime, &begintime) )
                                {
                                        move(statline, 0);
                                        clrtoeol();
                                        statmsg = "Wrong time format!";
                                        beep();
					begintime = 0;
                                        break;
                                }

				goto free_and_return;	

			   /*
			   ** sort order automatically depending on
			   ** most busy resource
			   */
			   case MSORTAUTO:
				showorder = MSORTAUTO;
				firstitem = 0;
				break;

			   /*
			   ** sort in cpu-activity order
			   */
			   case MSORTCPU:
				showorder = MSORTCPU;
				firstitem = 0;
				break;

			   /*
			   ** sort in memory-consumption order
			   */
			   case MSORTMEM:
				showorder = MSORTMEM;
				firstitem = 0;
				break;

			   /*
			   ** sort in disk-activity order
			   */
			   case MSORTDSK:
				showorder = MSORTDSK;
				firstitem = 0;
				break;

			   /*
			   ** sort in network-activity order
			   */
			   case MSORTNET:
				if ( !(supportflags & NETATOP || supportflags & NETATOPBPF))
				{
					statmsg = "Ignored: 'netatop' or 'netatop-bpf' not "
					          "active, no -K specified or no root privs";
					break;
				}
				showorder = MSORTNET;
				firstitem = 0;
				break;

			   /*
			   ** sort in gpu-activity order
			   */
			   case MSORTGPU:
				if ( !(supportflags & GPUSTAT) )
				{
					statmsg = "Ignored: no GPU daemon running or "
					          "no -k specified";
					break;
				}
				showorder = MSORTGPU;
				firstitem = 0;
				break;

			   /*
			   ** general figures per process
			   */
			   case MPROCGEN:
				showtype  = MPROCGEN;

				if (showorder != MSORTAUTO)
					showorder = MSORTCPU;

				firstitem = 0;
				break;

			   /*
			   ** memory-specific figures per process
			   */
			   case MPROCMEM:
				showtype  = MPROCMEM;

				if (showorder != MSORTAUTO)
					showorder = MSORTMEM;

				firstitem = 0;
				break;

			   /*
			   ** disk-specific figures per process
			   */
			   case MPROCDSK:
				showtype  = MPROCDSK;

				if (showorder != MSORTAUTO)
					showorder = MSORTDSK;

				firstitem = 0;
				break;

			   /*
			   ** network-specific figures per process
			   */
			   case MPROCNET:
				if ( !(supportflags & NETATOP || supportflags & NETATOPBPF) )
				{
					statmsg = "Ignored: 'netatop' or 'netatop-bpf' not "
					          "active, no -K specified or no root privs";
					break;
				}

				showtype  = MPROCNET;

				if (showorder != MSORTAUTO)
					showorder = MSORTNET;

				firstitem = 0;
				break;

			   /*
			   ** GPU-specific figures per process
			   */
			   case MPROCGPU:
				if ( !(supportflags & GPUSTAT) )
				{
					statmsg = "Ignored: no GPU daemon running or "
					          "no -k specified";
					break;
				}

				showtype  = MPROCGPU;

				if (showorder != MSORTAUTO)
					showorder = MSORTGPU;

				firstitem = 0;
				break;

			   /*
			   ** various info per process
			   */
			   case MPROCVAR:
				showtype  = MPROCVAR;
				firstitem = 0;
				break;

			   /*
			   ** command line per process
			   */
			   case MPROCARG:
				showtype  = MPROCARG;
				firstitem = 0;
				break;

			   /*
			   ** cgroup v2 info per process
			   */
			   case MCGROUPS:
				if ( !(supportflags & CGROUPV2) )
				{
					statmsg = "No cgroup v2 metrics "
					          "available; request ignored!";
					break;
				}

				showtype  = MCGROUPS;
				firstitem = 0;
				break;

			   case '2':
			   case '3':
			   case '4':
			   case '5':
			   case '6':
			   case '7':
			   case '8':
			   case '9':
				cgroupdepth = lastchar - 0x30;
				firstitem = 0;
				break;

			   /*
			   ** own defined output per process
			   */
			   case MPROCOWN:
				if (! ownprocs[0].pf)
				{
					statmsg = "Own process line is not "
					          "configured in rc-file; "
					          "request ignored";
					break;
				}

				showtype  = MPROCOWN;
				firstitem = 0;
				break;

			   /*
			   ** scheduling-values per process
			   */
			   case MPROCSCH:
				showtype  = MPROCSCH;

				if (showorder != MSORTAUTO)
					showorder = MSORTCPU;

				firstitem = 0;
				break;

			   /*
			   ** accumulated resource consumption per user
			   */
			   case MCUMUSER:
				statmsg = "Consumption per user; use 'a' to "
				          "toggle between all/active processes";

				showtype  = MCUMUSER;
				firstitem = 0;
				break;

			   /*
			   ** accumulated resource consumption per program
			   */
			   case MCUMPROC:
				statmsg = "Consumption per program; use 'a' to "
				          "toggle between all/active processes";

				showtype  = MCUMPROC;
				firstitem = 0;
				break;

			   /*
			   ** accumulated resource consumption per container/pod
			   */
			   case MCUMCONT:
				if (!rawreadflag && !rootprivs())
				{
					statmsg = "No privileges to get "
					          "container/pod identity!";
					break;
				}

				statmsg = "Consumption per container/pod; use 'a' to "
				          "toggle between all/active processes";

				showtype  = MCUMCONT;
				firstitem = 0;
				break;

			   /*
			   ** help wanted?
			   */
			   case MHELP1:
			   case MHELP2:
				alarm(0);	/* stop the clock   */

				move(1, 0);
				clrtobot();	/* blank the screen */
				refresh();

				showhelp(2);

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(1); /* force new sample */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					// jump to last sample after awaiting input
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}

				break;

			   /*
			   ** send signal to process
			   */
			   case MKILLPROC:
				if (rawreadflag && !twinpid)
				{
					statmsg = "Not possible when viewing "
					          "raw file!";
					beep();
					break;
				}

				alarm(0);	/* stop the clock */

				killpid = getnumval("Pid of process: ",
						     0, statline);

				switch (killpid)
				{
				   case 0:
				   case -1:
					break;

				   case 1:
					statmsg = "Sending signal to pid 1 not "
					          "allowed!";
					beep();
					break;

				   default:
					clrtoeol();
					killsig = getnumval("Signal [%d]: ",
						     15, statline);

					if ( kill(killpid, killsig) == -1)
					{
						statmsg = "Not possible to "
						     "send signal to this pid!";
						beep();
					}
				}

				if (!paused && !twinpid)
					alarm(3); /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** change interval timeout
			   */
			   case MINTERVAL:
				if (rawreadflag)
				{
					statmsg = "Not possible in twin mode or "
						  "when viewing raw file!";
					beep();
					break;
				}

				alarm(0);	/* stop the clock */

				interval = getnumval("New interval in seconds "
						     "(now %d): ",
						     interval, statline);

				if (interval)
				{
					if (!paused)
						alarm(1); /* set short timer */
				}
				else
				{
					statmsg = "No timer set; waiting for "
					          "manual trigger ('t').....";
				}

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific user
			   */
			   case MSELUSER:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Users as regular expression or "
				       "one numerical UID (enter=all users): ");

				procsel.username[0] = '\0';
				scanw("%255s\n", procsel.username);

				noecho();

				if (procsel.username[0]) /* data entered ? */
				{
					regex_t		userregex;
					int		u = 0;

					if ( regcomp(&userregex,
						procsel.username, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						procsel.username[0] = '\0';
					}
					else
					{
						while ( (pwd = getpwent()))
						{
							if (regexec(&userregex,
							    pwd->pw_name, 0,
							    NULL, 0))
								continue;

							if (u < MAXUSERSEL-1)
							{
							  procsel.userid[u] =
								pwd->pw_uid;
							  u++;
							}
						}
						endpwent();

						procsel.userid[u] = USERSTUB;

						if (u == 0)
						{
							/*
							** possibly a numerical
							** value specified?
							*/
							if (numeric(
							     procsel.username))
							{
							 procsel.userid[0] =
							 atoi(procsel.username);
							 procsel.userid[1] =
								USERSTUB;
							}
							else
							{
							     statmsg =
								"No user-names "
							    	"match this "
								"pattern!";
							     beep();
							}
						}
					}
				}
				else
				{
					procsel.userid[0] = USERSTUB;
				}

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific process-name
			   */
			   case MSELPROC:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Process-name as regular "
				       "expression (enter=no regex): ");

				procsel.prognamesz  = 0;
				procsel.progname[0] = '\0';

				scanw("%63s\n", procsel.progname);
				procsel.prognamesz = strlen(procsel.progname);

				if (procsel.prognamesz)
				{
					if (regcomp(&procsel.progregex,
					         procsel.progname, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						procsel.prognamesz  = 0;
						procsel.progname[0] = '\0';
					}
				}

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific container/pod id
			   */
			   case MSELCONT:
				if (!rawreadflag && !rootprivs())
				{
					statmsg = "No privileges to get "
					          "container/pod identity!";
					beep();
					break;
				}

				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Container or pod name (enter=all, "
				       "'host'=host processes): ");

				procsel.utsname[0]  = '\0';
				scanw("%15s", procsel.utsname);
				procsel.utsname[UTSLEN] = '\0';

				switch (strlen(procsel.utsname))
				{
                                   case 0:
					break;	// enter key pressed

				   case 4:	// host?
					if (strcmp(procsel.utsname, "host") == 0)
					{
						procsel.utsname[0] = 'H';
						procsel.utsname[1] = '\0';
					}
					break;
				}

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific PIDs
			   */
			   case MSELPID:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Comma-separated PIDs of processes "
				                 "(enter=no selection): ");

				scanw("%79s\n", genline);

				int  id = 0;

				char *pidp = strtok(genline, ",");

				while (pidp)
				{
					char *ep;

					if (id >= MAXPID-1)
					{
						procsel.pid[id] = 0;	// stub

						statmsg = "Maximum number of"
						          "PIDs reached!";
						beep();
						break;
					}

					procsel.pid[id] = strtol(pidp, &ep, 10);

					if (*ep)
					{
						statmsg = "Non-numerical PID!";
						beep();
						procsel.pid[0]  = 0;  // stub
						break;
					}

					id++;
					pidp = strtok(NULL, ",");
				}

				procsel.pid[id] = 0;	// stub

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific process/thread state
			   */
			   case MSELSTATE:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();

				/* Linux fs/proc/array.c - task_state_array */
				printw("Comma-separated thread states within process "
				       "(R|S|D|I|T|t|X|Z|P): ");

				memset(procsel.states, 0, sizeof procsel.states);

				scanw("%15s\n", genline);

				char *sp = strtok(genline, ",");

				while (sp && *sp)
				{
					if (isspace(*sp))
					{
						sp++;
						continue;
					}

					if (strlen(sp) > 1)
					{
						statmsg = "Invalid state!";
						memset(procsel.states, 0,
							sizeof procsel.states);
						break;
					}

					int needed = 0;

					switch (*sp)
					{
						case 'R': /* running */
						case 'S': /* sleeping */
						case 'D': /* disk sleep */
						case 'I': /* idle */
						case 'T': /* stopped */
						case 't': /* tracing stop */
						case 'X': /* dead */
						case 'Z': /* zombie */
						case 'P': /* parked */
							if (!strchr(procsel.states, *sp))
								needed = 1;
							break;
						default:
							statmsg = "Invalid state!";
							memset(procsel.states,
								0, sizeof procsel.states);
							beep();
							break;
					}

					if (needed)
					    procsel.states[strlen(procsel.states)] = *sp;

					sp = strtok(NULL, ",");
				}

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific command line arguments
			   */
			   case MSELARG:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Command line string as regular "
				       "expression (enter=no regex): ");

				procsel.argnamesz  = 0;
				procsel.argname[0] = '\0';

				scanw("%63s\n", procsel.argname);
				procsel.argnamesz = strlen(procsel.argname);

				if (procsel.argnamesz)
				{
					if (regcomp(&procsel.argregex,
					         procsel.argname, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						procsel.argnamesz  = 0;
						procsel.argname[0] = '\0';
					}
				}

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** focus on specific system resource
			   */
			   case MSELSYS:
				alarm(0);	/* stop the clock */
				echo();

				move(statline, 0);
				clrtoeol();
				printw("Logical volume name as regular "
				       "expression (enter=no specific name): ");

				syssel.lvmnamesz  = 0;
				syssel.lvmname[0] = '\0';

				scanw("%63s\n", syssel.lvmname);
				syssel.lvmnamesz = strlen(syssel.lvmname);

				if (syssel.lvmnamesz)
				{
					if (regcomp(&syssel.lvmregex,
					         syssel.lvmname, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						syssel.lvmnamesz  = 0;
						syssel.lvmname[0] = '\0';
					}
				}

				move(statline, 0);
				clrtoeol();
				printw("Disk name as regular "
				       "expression (enter=no specific name): ");

				syssel.dsknamesz  = 0;
				syssel.dskname[0] = '\0';

				scanw("%63s\n", syssel.dskname);
				syssel.dsknamesz = strlen(syssel.dskname);

				if (syssel.dsknamesz)
				{
					if (regcomp(&syssel.dskregex,
					         syssel.dskname, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						syssel.dsknamesz  = 0;
						syssel.dskname[0] = '\0';
					}
				}

				move(statline, 0);
				clrtoeol();
				printw("Interface name as regular "
				       "expression (enter=no specific name): ");

				syssel.itfnamesz  = 0;
				syssel.itfname[0] = '\0';

				scanw("%63s\n", syssel.itfname);
				syssel.itfnamesz = strlen(syssel.itfname);

				if (syssel.itfnamesz)
				{
					if (regcomp(&syssel.itfregex,
					         syssel.itfname, REG_NOSUB))
					{
						statmsg = "Invalid regular "
						          "expression!";
						beep();

						syssel.itfnamesz  = 0;
						syssel.itfname[0] = '\0';
					}
				}

				noecho();

				move(statline, 0);

				if (interval && !paused && !rawreadflag)
					alarm(3);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** toggle pause-state
			   */
			   case MPAUSE:
				if (rawreadflag && !twinpid)
				{
					statmsg = "Just use 'T' and 't' to browse...";
					break;
				}

				if (paused)
				{
					paused=0;
					clrtoeol();
					refresh();

					if (!rawreadflag)
					{
						alarm(1);	/* start the clock */
					}
					else
					{
						begintime = 0x7fffffff;
						lastchar = MSAMPBRANCH;
						goto free_and_return;	
					}
				}
				else
				{
					paused=1;
					clrtoeol();
					refresh();
					alarm(0);	/* stop the clock */
				}
				break;

			   /*
			   ** toggle between modified processes and
			   ** all processes, or between used cgroups and 
			   ** all cgroups
			   */
			   case MALLACTIVE:
				if (deviatonly)
				{
					deviatonly=0;
					statmsg = "All processes/threads/cgroups will be "
					          "shown/accumulated...";
				}
				else
				{
					deviatonly=1;
					statmsg = "Only active processes/threads/cgroups "
					          "will be shown/accumulated...";
				}

				tlastorder = 0;
				firstitem  = 0;
				break;

			   /*
			   ** toggle average or total values
			   */
			   case MAVGVAL:
				if (avgval)
					avgval=0;
				else
					avgval=1;
				break;

			   /*
			   ** system-statistics lines:
			   **	         toggle fixed or variable
			   */
			   case MSYSFIXED:
				if (fixedhead)
				{
					fixedhead=0;
					statmsg = "Only active system-resources"
					          " will be shown ......";
				}
				else
				{
					fixedhead=1;
					statmsg = "Also inactive "
					  "system-resources will be shown.....";
				}

				firstitem = 0;
				break;

			   /*
			   ** system-statistics lines:
			   **	         toggle fixed or variable
			   */
			   case MSYSNOSORT:
				if (sysnosort)
				{
					sysnosort=0;
					statmsg = "System resources will be "
					          "sorted on utilization...";
				}
				else
				{
					sysnosort=1;
					statmsg = "System resources will not "
					          "be sorted on utilization...";
				}

				firstitem = 0;
				break;

			   /*
			   ** per-thread view wanted with sorting on
			   ** process level
			   */
			   case MTHREAD:
				if (threadview)
				{
					threadview = 0;
					statmsg    = "Thread view disabled";
					firstitem  = 0;
				}
				else
				{
					threadview = 1;
					statmsg    = "Thread view enabled";
					firstitem  = 0;
				}
				break;

			   /*
			   ** sorting on thread level as well (threadview)
			   */
			   case MTHRSORT:
				if (threadsort)
				{
					threadsort = 0;
					statmsg    = "Thread sorting disabled for thread view";
					firstitem  = 0;
				}
				else
				{
					threadsort = 1;
					statmsg    = "Thread sorting enabled for thread view";
					firstitem  = 0;
				}
				break;

			   /*
			   ** per-process PSS calculation wanted 
			   */
			   case MCALCPSS:
				if (rawreadflag)
				{
					statmsg = "PSIZE gathering depends "
					          "on rawfile";
					break;
				}

				if (calcpss)
				{
					calcpss    = 0;
					statmsg    = "PSIZE gathering disabled";
				}
				else
				{
					calcpss    = 1;

					if (rootprivs())
						statmsg    = "PSIZE gathering enabled";
					else
						statmsg    = "PSIZE gathering only "
						             "for own processes";
				}
				break;

			   /*
			   ** per-thread WCHAN definition 
			   */
			   case MGETWCHAN:
				if (getwchan)
				{
					getwchan   = 0;
					statmsg    = "WCHAN gathering disabled";
				}
				else
				{
					getwchan   = 1;
					statmsg    = "WCHAN gathering enabled";
				}
				break;

			   /*
			   ** suppression of terminated processes in output
			   */
			   case MSUPEXITS:
				if (suppressexit)
				{
					suppressexit = 0;
					statmsg      = "Exited processes will "
					               "be shown/accumulated";
					firstitem    = 0;
				}
				else
				{
					suppressexit = 1;
					statmsg      = "Exited processes will "
					             "not be shown/accumulated";
					firstitem    = 0;
				}
				break;

			   /*
			   ** screen lines:
			   **	         toggle for colors
			   */
			   case MCOLORS:
				if (usecolors)
				{
					usecolors=0;
					statmsg = "No colors will be used...";
				}
				else
				{
					if (screen && has_colors())
					{
						usecolors=1;
						statmsg =
						   "Colors will be used...";
					}
					else
					{
						statmsg="No colors supported!";
					}
				}

				firstitem = 0;
				break;

			   /*
			   ** system-statistics lines:
			   **	         toggle no or all active disk
			   */
			   case MSYSLIMIT:
				alarm(0);	/* stop the clock */

				maxcpulines =
				  getnumval("Maximum lines for per-cpu "
				            "statistics (now %d): ",
				            maxcpulines, statline);

				maxgpulines =
				  getnumval("Maximum lines for per-gpu "
				            "statistics (now %d): ",
				            maxgpulines, statline);

				if (sstat->dsk.nlvm > 0)
				{
					maxlvmlines =
					  getnumval("Maximum lines for LVM "
				            "statistics (now %d): ",
				            maxlvmlines, statline);
				}

				if (sstat->dsk.nmdd > 0)
				{
			  		maxmddlines =
					  getnumval("Maximum lines for MD "
					    "device statistics (now %d): ",
				            maxmddlines, statline);
				}

				maxdsklines =
				  getnumval("Maximum lines for disk "
				            "statistics (now %d): ",
				            maxdsklines, statline);

				maxintlines =
				  getnumval("Maximum lines for interface "
				            "statistics (now %d): ",
					    maxintlines, statline);

				maxifblines =
				  getnumval("Maximum lines for infiniband "
				            "port statistics (now %d): ",
					    maxifblines, statline);

				maxnfslines =
				  getnumval("Maximum lines for NFS mount "
				            "statistics (now %d): ",
					    maxnfslines, statline);

				maxcontlines =
				  getnumval("Maximum lines for container "
				            "statistics (now %d): ",
					    maxcontlines, statline);

				maxnumalines =
				  getnumval("Maximum lines for numa "
				            "statistics (now %d): ",
					    maxnumalines, statline);

				maxllclines =
				  getnumval("Maximum lines for LLC "
				            "statistics (now %d): ",
					    maxllclines, statline);

				if (interval && !paused && !rawreadflag)
					alarm(1);  /* set short timer */

				firstitem = 0;

				if (twinpid)	// twin mode?
				{
					begintime = 0x7fffffff;
					lastchar = MSAMPBRANCH;
					goto free_and_return;	
				}
				break;

			   /*
			   ** reset counters 
			   */
			   case MRESET:
				getalarm(0);	/* restart the clock */
				paused = 0;

				if (twinpid)
				{
					paused=1;	// implicit pause in twin mode
					clrtoeol();
					refresh();
				}

				goto free_and_return;	

			   /*
			   ** branch to end of log 
			   */
			   case MEND:
                                if (!rawreadflag)
                                {
					statmsg = "Only allowed in twin mode or "
						  "when viewing raw file!";
                                        beep();
                                        break;
                                }

				if (!paused && twinpid)
				{
					paused=1;	// implicit pause in twin mode
					clrtoeol();
					refresh();
				}

				goto free_and_return;	

			   /*
			   ** show version info
			   */
			   case MVERSION:
				statmsg = getstrvers();
				break;

			   /*
			   ** handle redraw request
			   */
			   case MREDRAW:
                                wclear(stdscr);
				break;

			   /*
			   ** handle arrow right for command line
			   */
			   case KEY_RIGHT:
				startoffset++;
				break;

			   /*
			   ** handle arrow left for command line
			   */
			   case KEY_LEFT:
				if (startoffset > 0)
					startoffset--;
				break;

			   /*
			   ** handle arrow down to go one line down
			   */
			   case KEY_DOWN:
				if (firstitem < alistsz-1)
					firstitem += 1;
				break;

			   /*
			   ** handle arrow up to go one line up
			   */
			   case KEY_UP:	
				if (firstitem > 0)
					firstitem -= 1;
				break;

			   /*
			   ** handle forward
			   */
			   case KEY_NPAGE:
			   case MLISTFW:
				if (alistsz-firstitem > slistsz)
					firstitem += slistsz;
				break;

			   /*
			   ** handle backward
			   */
			   case KEY_PPAGE:
			   case MLISTBW:
				if (firstitem >= slistsz)
					firstitem -= slistsz;
				else
					firstitem = 0;
				break;

			   /*
			   ** handle screen resize
			   */
			   case KEY_RESIZE:
				snprintf(statbuf, sizeof statbuf, 
					"Window resized to %dx%d...",
			         		COLS, LINES);
				statmsg = statbuf;

				timeout(0);
				(void) getch();
				timeout(-1);
				break;

			   /*
			   ** unknown key-stroke
			   */
			   default:
			        beep();
			}
		}
		else	/* no screen */
		{
			lastchar = '\0';
			goto free_and_return;
		}
	}

    free_and_return:
	free(tpcumlist);
	free(pcumlist);
	free(tucumlist);
	free(ucumlist);
	free(tccumlist);
	free(ccumlist);
	free(tsklist);
	free(sellist);
	free(cgroupsort);
	free(cgroupsel);

	return lastchar;
}

/*
** accumulate all processes per user in new list
*/
static int
cumusers(struct tstat **curprocs, struct tstat *curusers, int numprocs)
{
	register int	i, numusers;

	/*
	** sort list of active processes in order of uid (increasing)
	*/
	qsort(curprocs, numprocs, sizeof(struct tstat *), compusr);

	/*
	** accumulate all processes per user in the new list
	*/
	for (numusers=i=0; i < numprocs; i++, curprocs++)
	{
		if (procsuppress(*curprocs, &procsel))
			continue;

		if ((*curprocs)->gen.state == 'E' && suppressexit)
			continue;
 
		if ( curusers->gen.ruid != (*curprocs)->gen.ruid )
		{
			if (curusers->gen.pid)
			{
				numusers++;
				curusers++;
			}
			curusers->gen.ruid = (*curprocs)->gen.ruid;
		}

		accumulate(*curprocs, curusers);
	}

	if (curusers->gen.pid)
		numusers++;

	return numusers;
}


/*
** accumulate all processes with the same name (i.e. same program)
** into a new list
*/
static int
cumprogs(struct tstat **curprocs, struct tstat *curprogs, int numprocs)
{
	register int	i, numprogs;

	/*
	** sort list of active processes in order of process-name
	*/
	qsort(curprocs, numprocs, sizeof(struct tstat *), compnam);

	/*
	** accumulate all processes with same name in the new list
	*/
	for (numprogs=i=0; i < numprocs; i++, curprocs++)
	{
		if (procsuppress(*curprocs, &procsel))
			continue;

		if ((*curprocs)->gen.state == 'E' && suppressexit)
			continue;

		if ( strcmp(curprogs->gen.name, (*curprocs)->gen.name) != 0)
		{
			if (curprogs->gen.pid)
			{
				numprogs++;
				curprogs++;
			}
			strcpy(curprogs->gen.name, (*curprocs)->gen.name);
		}

		accumulate(*curprocs, curprogs);
	}

	if (curprogs->gen.pid)
		numprogs++;

	return numprogs;
}

/*
** accumulate all processes per container/pod in new list
*/
static int
cumconts(struct tstat **curprocs, struct tstat *curconts, int numprocs)
{
	register int	i, numconts;

	/*
	** sort list of active processes in order of container/pod (increasing)
	*/
	qsort(curprocs, numprocs, sizeof(struct tstat *), compcon);

	/*
	** accumulate all processes per container/pod in the new list
	*/
	for (numconts=i=0; i < numprocs; i++, curprocs++)
	{
		if (procsuppress(*curprocs, &procsel))
			continue;

		if ((*curprocs)->gen.state == 'E' && suppressexit)
			continue;
 
		if ( strcmp(curconts->gen.utsname,
                         (*curprocs)->gen.utsname) != 0)
		{
			if (curconts->gen.pid)
			{
				numconts++;
				curconts++;
			}
			strcpy(curconts->gen.utsname,
			    (*curprocs)->gen.utsname);
		}

		accumulate(*curprocs, curconts);
	}

	if (curconts->gen.pid)
		numconts++;

	return numconts;
}


/*
** accumulate relevant counters from individual task to
** combined task
*/
static void
accumulate(struct tstat *curproc, struct tstat *curstat)
{
	count_t		nett_wsz;

	curstat->gen.pid++;		/* misuse as counter */

	curstat->gen.isproc  = 1;
	curstat->gen.nthr   += curproc->gen.nthr;
	curstat->cpu.utime  += curproc->cpu.utime;
	curstat->cpu.stime  += curproc->cpu.stime;
	curstat->cpu.nvcsw  += curproc->cpu.nvcsw;
	curstat->cpu.nivcsw += curproc->cpu.nivcsw;
	curstat->cpu.rundelay += curproc->cpu.rundelay;
	curstat->cpu.blkdelay += curproc->cpu.blkdelay;

	if (curproc->dsk.wsz > curproc->dsk.cwsz)
               	nett_wsz = curproc->dsk.wsz -curproc->dsk.cwsz;
	else
		nett_wsz = 0;

	curstat->dsk.rio    += curproc->dsk.rsz;
	curstat->dsk.wio    += nett_wsz;

	curstat->dsk.rsz     = curstat->dsk.rio;
	curstat->dsk.wsz     = curstat->dsk.wio;

	curstat->net.tcpsnd += curproc->net.tcpsnd;
	curstat->net.tcprcv += curproc->net.tcprcv;
	curstat->net.udpsnd += curproc->net.udpsnd;
	curstat->net.udprcv += curproc->net.udprcv;

	curstat->net.tcpssz += curproc->net.tcpssz;
	curstat->net.tcprsz += curproc->net.tcprsz;
	curstat->net.udpssz += curproc->net.udpssz;
	curstat->net.udprsz += curproc->net.udprsz;

	if (curproc->gen.state != 'E')
	{
		if  (curproc->mem.pmem != -1)  // no errors?
			curstat->mem.pmem += curproc->mem.pmem;

		curstat->mem.vmem   += curproc->mem.vmem;
		curstat->mem.rmem   += curproc->mem.rmem;
		curstat->mem.vlibs  += curproc->mem.vlibs;
		curstat->mem.vdata  += curproc->mem.vdata;
		curstat->mem.vstack += curproc->mem.vstack;
		curstat->mem.vswap  += curproc->mem.vswap;
		curstat->mem.vlock  += curproc->mem.vlock;
		curstat->mem.rgrow  += curproc->mem.rgrow;
		curstat->mem.vgrow  += curproc->mem.vgrow;

		if (curproc->gpu.state)		// GPU is use?
		{
			int i;

			curstat->gpu.state = 'A';

			if (curproc->gpu.gpubusy == -1)
				curstat->gpu.gpubusy  = -1;
			else
				curstat->gpu.gpubusy += curproc->gpu.gpubusy;

			if (curproc->gpu.membusy == -1)
				curstat->gpu.membusy  = -1;
			else
				curstat->gpu.membusy += curproc->gpu.membusy;

			curstat->gpu.memnow  += curproc->gpu.memnow;
			curstat->gpu.gpulist |= curproc->gpu.gpulist;
			curstat->gpu.nrgpus   = 0;

			for (i=0; i < MAXGPU; i++)
			{
				if (curstat->gpu.gpulist & 1<<i)
					curstat->gpu.nrgpus++;
			}
		}
	}
}


/*
** function that checks if the current process or thread must be
** selected or suppressed
**
** returns 1 (suppress) or 0 (do not suppress)
*/
static int
procsuppress(struct tstat *curstat, struct pselection *sel)
{
	/*
	** check if only processes of a particular user
	** should be shown
	*/
	if (sel->userid[0] != USERSTUB)
	{
		int     u = 0;

		while (sel->userid[u] != USERSTUB)
		{
			if (sel->userid[u] == curstat->gen.ruid)
				break;
			u++;
		}

		if (sel->userid[u] != curstat->gen.ruid)
			return 1;
	}

	/*
	** check if only processes with particular PIDs
	** should be shown
	*/
	if (sel->pid[0])
	{
		int i = 0;

		while (sel->pid[i])
		{
			if (sel->pid[i] == curstat->gen.tgid)
				break;
			i++;
		}

		if (sel->pid[i] != curstat->gen.tgid)
			return 1;
	}

	/*
	** check if only processes with a particular name
	** should be shown
	*/
	if (sel->prognamesz &&
	    regexec(&(sel->progregex), curstat->gen.name, 0, NULL, 0))
		return 1;

	/*
	** check if only processes with a particular command line string
	** should be shown
	*/
	if (sel->argnamesz)
	{
		if (curstat->gen.cmdline[0])
		{
			if (regexec(&(sel->argregex), curstat->gen.cmdline,
								0, NULL, 0))
				return 1;
		}
		else
		{
			if (regexec(&(sel->argregex), curstat->gen.name,
								0, NULL, 0))
				return 1;
		}
	}

	/*
	** check if only processes related to a particular container/pod
	** should be shown (container/pod 'H' stands for native host processes)
	*/
	if (sel->utsname[0])
	{
		if (sel->utsname[0] == 'H')	// only host processes
		{
			if (curstat->gen.utsname[0])
				return 1;
		}
		else
		{
			if (memcmp(sel->utsname, curstat->gen.utsname, 12))
				return 1;
		}
	}

	/*
	** check if only processes in specific states should be shown 
	**
	** notice that the state of a process (i.e. the main thread)
	** may be 'S' while a thread in the process might have state 'R'
	** --> still show the process in that case!
	*/
	if (sel->states[0])
	{
		// check the states of the threads of this process
		//
		if (strchr(sel->states, 'R') && curstat->gen.nthrrun)
			return 0;

		if (strchr(sel->states, 'S') && curstat->gen.nthrslpi)
			return 0;

		if (strchr(sel->states, 'D') && curstat->gen.nthrslpu)
			return 0;

		if (strchr(sel->states, 'I') && curstat->gen.nthridle)
			return 0;

		// check the state of the process itself
		//
		if (strchr(sel->states, curstat->gen.state) == NULL)
			return 1;
	}

	return 0;
}


static void
limitedlines(void)
{
	if (maxcpulines == 999)		// default?
		maxcpulines  = 0;

	if (maxgpulines == 999)		// default?
		maxgpulines  = 2;

	if (maxdsklines == 999)		// default?
		maxdsklines  = 3;

	if (maxmddlines == 999)		// default?
		maxmddlines  = 3;

	if (maxlvmlines == 999)		// default?
		maxlvmlines  = 4;

	if (maxintlines == 999)		// default?
		maxintlines  = 2;

	if (maxifblines == 999)		// default?
		maxifblines  = 2;

	if (maxnfslines == 999)		// default?
		maxnfslines  = 2;

	if (maxcontlines == 999)	// default?
		maxcontlines = 1;

	if (maxnumalines == 999)	// default?
		maxnumalines = 0;

	if (maxllclines  == 999)	// default?
		maxllclines = 0;
}

/*
** get a numerical value from the user and verify 
*/
static long
getnumval(char *ask, long valuenow, int statline)
{
	char numval[16];
	long retval;

	echo();
	move(statline, 0);
	clrtoeol();
	printw(ask, valuenow);

	numval[0] = 0;
	scanw("%15s", numval);

	move(statline, 0);
	noecho();

	if (numval[0])  /* data entered ? */
	{
		if ( numeric(numval) )
		{
			retval = atol(numval);
		}
		else
		{
			beep();
			clrtoeol();
			printw("Value not numeric (current value kept)!");
			refresh();
			sleep(2);
			retval = valuenow;
		}
	}
	else
	{
		retval = valuenow;
	}

	return retval;
}

/*
** generic print-function which checks if printf should be used
** (to file or pipe) or curses (to screen)
*/
void
printg(const char *format, ...)
{
	va_list	args;

	va_start(args, format);

	if (screen)
		vw_printw(stdscr, (char *) format, args);
	else
		vprintf(format, args);

	va_end(args);
}

/*
** initialize generic sample output functions
*/
static void
generic_init(void)
{
	int i;

	/*
	** check if default sort order and/or showtype are overruled
	** by command-line flags
	*/
	for (i=0; flaglist[i]; i++)
	{
		switch (flaglist[i])
		{
		   case MSORTAUTO:
			showorder = MSORTAUTO;
			break;

		   case MSORTCPU:
			showorder = MSORTCPU;
			break;

		   case MSORTGPU:
			showorder = MSORTGPU;
			break;

		   case MSORTMEM:
			showorder = MSORTMEM;
			break;

		   case MSORTDSK:
			showorder = MSORTDSK;
			break;

		   case MSORTNET:
			showorder = MSORTNET;
			break;

		   case MPROCGEN:
			showtype  = MPROCGEN;
			showorder = MSORTCPU;
			break;

		   case MPROCGPU:
			showtype  = MPROCGPU;
			showorder = MSORTGPU;
			break;

		   case MPROCMEM:
			showtype  = MPROCMEM;
			showorder = MSORTMEM;
			break;

		   case MPROCSCH:
			showtype  = MPROCSCH;
			showorder = MSORTCPU;
			break;

		   case MPROCDSK:
			showtype  = MPROCDSK;
			showorder = MSORTDSK;
			break;

		   case MPROCNET:
			if ( !(supportflags & NETATOP || supportflags & NETATOPBPF) )
			{
				fprintf(stderr, "Ignored: 'netatop' or 'netatop-bpf' not "
					        "active, no -K specified or no root privs");
				sleep(3);
				break;
			}

			showtype  = MPROCNET;
			showorder = MSORTNET;
			break;

		   case MPROCVAR:
			showtype  = MPROCVAR;
			break;

		   case MPROCARG:
			showtype  = MPROCARG;
			break;

		   case MCGROUPS:
			if ( !(supportflags & CGROUPV2) )
			{
				fprintf(stderr, "No cgroup v2 metrics "
				          "available; request ignored!\n");
				sleep(3);
				break;
			}

			showtype  = MCGROUPS;
			break;

		   case MPROCOWN:
			showtype  = MPROCOWN;
			break;

		   case MAVGVAL:
			if (avgval)
				avgval=0;
			else
				avgval=1;
			break;

		   case MCUMUSER:
			showtype  = MCUMUSER;
			break;

		   case MCUMPROC:
			showtype  = MCUMPROC;
			break;

		   case MCUMCONT:
			if (!rawreadflag && !rootprivs())
			{
				fprintf(stderr, "No privileges to get "
				                "container/pod identity!\n");
				sleep(3);
				break;
			}

			showtype  = MCUMCONT;
			break;

		   case MSYSFIXED:
			if (fixedhead)
				fixedhead=0;
			else
				fixedhead=1;
			break;

		   case MSYSNOSORT:
			if (sysnosort)
				sysnosort=0;
			else
				sysnosort=1;
			break;

		   case MTHREAD:
			if (threadview)
				threadview = 0;
			else
				threadview = 1;
			break;

		   case MTHRSORT:
			if (threadsort)
				threadsort = 0;
			else
				threadsort = 1;
			break;

		   case MCALCPSS:
			if (rawreadflag)
			{
				fprintf(stderr,
				        "PSIZE gathering depends on rawfile\n");
				sleep(3);
				break;
			}
			if (calcpss)
			{
				calcpss    = 0;
			}
			else
			{
				calcpss    = 1;

				if (!rootprivs())
				{
					fprintf(stderr,
				 	        "PSIZE gathering only for own "
					        "processes\n");
					sleep(3);
				}
			}
			break;

		   case MGETWCHAN:
			if (getwchan)
				getwchan = 0;
			else
				getwchan = 1;
			break;

		   case MSUPEXITS:
			if (suppressexit)
				suppressexit = 0;
			else
				suppressexit = 1;
			break;

		   case MCOLORS:
			if (usecolors)
				usecolors=0;
			else
				usecolors=1;
			break;

		   case MSYSLIMIT:
			limitedlines();
			break;

		   case '2':
		   case '3':
		   case '4':
		   case '5':
		   case '6':
		   case '7':
		   case '8':
		   case '9':
			cgroupdepth = flaglist[i] - 0x30;
			break;

		   default:
			prusage("atop");
		}
	}

       	/*
       	** set stdout output on line-basis
       	*/
       	setvbuf(stdout, (char *)0, _IOLBF, BUFSIZ);

       	/*
       	** check if STDOUT is related to a tty or
       	** something else (file, pipe)
       	*/
       	if ( isatty(fileno(stdout)) )
               	screen = 1;
       	else
             	screen = 0;

       	/*
       	** install catch-routine to finish in a controlled way
	** and activate cbreak mode
       	*/
       	if (screen)
	{
		/*
		** if stdin is not connected to a tty (might be redirected
		** to pipe or file), close it and duplicate stdout (tty)
		** to stdin
		*/
       		if ( !isatty(fileno(stdin)) )
		{
			(void) dup2(fileno(stdout), fileno(stdin));
		}

		/*
		** initialize screen-handling via curses
		*/
		setlocale(LC_ALL, "");
		setlocale(LC_NUMERIC, "C");

		initscr();
		cbreak();
		noecho();
		keypad(stdscr, TRUE);

		/*
		** verify minimal dimensions
		*/
		if (COLS  < MINCOLUMNS || LINES  < MINLINES)
		{
			endwin();       // finish ncurses interface

			fprintf(stderr,
				"Terminal size should be at least "
				"%d columns by %d lines\n",
				MINCOLUMNS, MINLINES);

			cleanstop(1);
		}

		if (has_colors())
		{
			use_default_colors();
			start_color();

			// color definitions
			//
			init_color(COLOR_MYORANGE, 675, 500,  50);
			init_color(COLOR_MYGREEN,    0, 600, 100);
			init_color(COLOR_MYGREY,   240, 240, 240);
			init_color(COLOR_MYLGREY,  800, 800, 800);

			init_color(COLOR_MYBROWN1, 420, 160, 160);
			init_color(COLOR_MYBROWN2, 735, 280, 280);

			init_color(COLOR_MYBLUE0,   50,  50, 300);
			init_color(COLOR_MYBLUE1,   50, 300, 500);
			init_color(COLOR_MYBLUE2,   50, 500, 800);
			init_color(COLOR_MYBLUE3,  100, 350, 600);
			init_color(COLOR_MYBLUE4,  150, 500, 700);
			init_color(COLOR_MYBLUE5,  200, 650, 800);

			init_color(COLOR_MYGREEN0,  90, 300,   0);
			init_color(COLOR_MYGREEN1,  90, 400, 100);
			init_color(COLOR_MYGREEN2,  90, 600, 100);

			// color pair definitions (foreground/background)
			//
			init_pair(FGCOLORINFO,     colorinfo,    -1);
			init_pair(FGCOLORALMOST,   coloralmost,  -1);
			init_pair(FGCOLORCRIT,     colorcrit,    -1);
			init_pair(FGCOLORTHR,      colorthread,  -1);
                	init_pair(FGCOLORBORDER,   COLOR_CYAN,   -1);
                	init_pair(FGCOLORGREY,     COLOR_MYLGREY, -1);

	                init_pair(WHITE_GREEN,     COLOR_WHITE, COLOR_MYGREEN);
			init_pair(WHITE_ORANGE,    COLOR_WHITE, COLOR_MYORANGE);
			init_pair(WHITE_RED,       COLOR_WHITE, COLOR_RED);
			init_pair(WHITE_GREY,      COLOR_WHITE, COLOR_MYGREY);
			init_pair(WHITE_BLUE,      COLOR_WHITE, COLOR_BLUE);
			init_pair(WHITE_MAGENTA,   COLOR_WHITE, COLOR_MAGENTA);

			init_pair(WHITE_BROWN1,    COLOR_WHITE, COLOR_MYBROWN1);
			init_pair(WHITE_BROWN2,    COLOR_WHITE, COLOR_MYBROWN2);

			init_pair(WHITE_BLUE0,     COLOR_WHITE, COLOR_MYBLUE0);
			init_pair(WHITE_BLUE1,     COLOR_WHITE, COLOR_MYBLUE1);
			init_pair(WHITE_BLUE2,     COLOR_WHITE, COLOR_MYBLUE2);
			init_pair(WHITE_BLUE3,     COLOR_WHITE, COLOR_MYBLUE3);
			init_pair(WHITE_BLUE4,     COLOR_WHITE, COLOR_MYBLUE4);
			init_pair(WHITE_BLUE5,     COLOR_WHITE, COLOR_MYBLUE5);

			init_pair(WHITE_GREEN0,    COLOR_WHITE, COLOR_MYGREEN0);
			init_pair(WHITE_GREEN1,    COLOR_WHITE, COLOR_MYGREEN1);
			init_pair(WHITE_GREEN2,    COLOR_WHITE, COLOR_MYGREEN2);
		}
		else
		{
			usecolors = 0;
		}
	}

	signal(SIGINT,   cleanstop);
	signal(SIGTERM,  cleanstop);
}




/*
** show help information in interactive mode
*/
static struct helptext {
	char *helpline;
	char helparg;
	char mode;	// 'l' - live, 'r' - rawlog, 'a' - all
} helptext[] = {
	{"Display mode:\n", ' ', 'a'},
	{"\t'%c'  - show bar graphs for system utilization (toggle)\n",
								MBARGRAPH, 'a'},
	{"\n",							' ', 'a'},
	{"Information in text mode about cgroups v2:\n", 	' ', 'a'},
	{"\t'%c'  - cgroups v2 metrics\n",			MCGROUPS, 'a'},
	{"\t 2-7 - cgroups tree level selection (default 7)\n", ' ', 'a'},
	{"\t 8   - cgroups with related processes, except kernel processes\n", ' ', 'a'},
	{"\t 9   - cgroups with related processes\n", ' ', 'a'},
	{"\t'%c'  - show all cgroups/processes i.s.o. only active ones (toggle)\n",
								MALLACTIVE, 'a'},
	{"\t'%c'  - sort on cpu activity\n",			MSORTCPU, 'a'},
	{"\t'%c'  - sort on memory utilization\n",		MSORTMEM, 'a'},
	{"\t'%c'  - sort on disk transfer rate\n",		MSORTDSK, 'a'},
	{"\n",							' ', 'a'},
	{"Information in text mode about processes:\n", 	' ', 'a'},
	{"\t'%c'  - generic info (default)\n",			MPROCGEN, 'a'},
	{"\t'%c'  - memory details\n",				MPROCMEM, 'a'},
	{"\t'%c'  - disk details\n",				MPROCDSK, 'a'},
	{"\t'%c'  - network details\n",				MPROCNET, 'a'},
	{"\t'%c'  - scheduling and thread-group info\n",	MPROCSCH, 'a'},
	{"\t'%c'  - GPU details\n",				MPROCGPU, 'a'},
	{"\t'%c'  - various info (ppid, user/group, date/time, status, "
	 "exitcode)\n",	MPROCVAR, 'a'},
	{"\t'%c'  - full command line per process\n",		MPROCARG, 'a'},
	{"\t'%c'  - use own output line definition\n",		MPROCOWN, 'a'},
	{"\n",							' ', 'a'},
	{"Sort list of processes in order of:\n",		' ', 'a'},
	{"\t'%c'  - cpu activity\n",				MSORTCPU, 'a'},
	{"\t'%c'  - memory consumption\n",			MSORTMEM, 'a'},
	{"\t'%c'  - disk activity\n",				MSORTDSK, 'a'},
	{"\t'%c'  - network activity\n",			MSORTNET, 'a'},
	{"\t'%c'  - GPU activity\n",				MSORTGPU, 'a'},
	{"\t'%c'  - most active system resource (auto mode)\n",	MSORTAUTO, 'a'},
	{"\n",							' ', 'a'},
	{"Raw file viewing and twin mode:\n",			' ', 'r'},
	{"\t'%c'  - show next     sample\n",			MSAMPNEXT, 'r'},
	{"\t'%c'  - show previous sample\n",			MSAMPPREV, 'r'},
	{"\t'%c'  - branch to certain time\n",			MSAMPBRANCH, 'r'},
	{"\t'%c'  - rewind to begin\n",				MRESET, 'r'},
	{"\t'%c'  - fast-forward to end\n",			MEND, 'r'},
	{"\t'%c'  - pause button to freeze or continue (twin mode)\n",
								MPAUSE, 'r'},
	{"\n",							' ', 'r'},
	{"Accumulated process figures:\n",			' ', 'a'},
	{"\t'%c'  - total resource consumption per user\n", 	MCUMUSER, 'a'},
	{"\t'%c'  - total resource consumption per program (i.e. same "
	 "process name)\n",					MCUMPROC, 'a'},
	{"\t'%c'  - total resource consumption per container/pod\n",MCUMCONT, 'a'},
	{"\n",							' ', 'a'},
	{"Process selections (keys shown in header line):\n",	' ', 'a'},
	{"\t'%c'  - focus on specific user name           "
	                              "(regular expression)\n", MSELUSER, 'a'},
	{"\t'%c'  - focus on specific program name        "
	                              "(regular expression)\n", MSELPROC, 'a'},
	{"\t'%c'  - focus on specific container/pod name\n",    MSELCONT, 'a'},
	{"\t'%c'  - focus on specific command line string "
	                              "(regular expression)\n", MSELARG, 'a'},
	{"\t'%c'  - focus on specific process id (PID)\n",      MSELPID, 'a'},
	{"\t'%c'  - focus on specific process/thread state(s)\n", MSELSTATE, 'a'},
	{"\n",							' ', 'a'},
	{"System resource selections (keys shown in header line):\n",' ', 'a'},
	{"\t'%c'  - focus on specific system resources    "
	                              "(regular expression)\n", MSELSYS, 'a'},
	{"\n",							      ' ', 'a'},
	{"Screen handling:\n",					      ' ', 'a'},
	{"\t^L   - redraw the screen                       \n",	      ' ', 'a'},
	{"\tPgDn - show next page in the process list (or ^F)\n",     ' ', 'a'},
	{"\tPgUp - show previous page in the process list (or ^B)\n", ' ', 'a'},
	{"\tArDn - arrow-down for next line in process list\n",       ' ', 'a'},
	{"\tArUp   arrow-up for previous line in process list\n",     ' ', 'a'},
	{"\tArRt - arrow-right for next character in full command line\n", ' ', 'a'},
	{"\tArLt - arrow-left for previous character in full command line\n",
									' ', 'a'},
	{"\n",							' ', 'a'},
	{"Presentation (keys shown in header line):\n",  	' ', 'a'},
	{"\t'%c'  - show all processes/threads (i.s.o. active)     (toggle)\n",
								MALLACTIVE, 'a'},
	{"\t'%c'  - show threads within process (thread view)      (toggle)\n",
		 						MTHREAD, 'a'},
	{"\t'%c'  - sort threads (when combined with thread view)  (toggle)\n",
		 						MTHRSORT, 'a'},
	{"\t'%c'  - show fixed number of header lines              (toggle)\n",
								MSYSFIXED, 'a'},
	{"\t'%c'  - suppress sorting system resources              (toggle)\n",
								MSYSNOSORT, 'a'},
	{"\t'%c'  - suppress terminated processes in output        (toggle)\n",
								MSUPEXITS, 'a'},
	{"\t'%c'  - no colors to indicate high occupation          (toggle)\n",
								MCOLORS, 'a'},
	{"\t'%c'  - show average-per-second i.s.o. total values    (toggle)\n",
								MAVGVAL, 'a'},
	{"\t'%c'  - calculate proportional set size (PSIZE)        (toggle)\n",
								MCALCPSS, 'a'},
	{"\t'%c'  - determine WCHAN per thread                     (toggle)\n",
								MGETWCHAN, 'a'},
	{"\n",							' '},
	{"Miscellaneous commands:\n",				' '},
	{"\t'%c'  - change interval timer (0 = only manual trigger)\n",
								MINTERVAL, 'l'},
	{"\t'%c'  - manual trigger to force next sample\n",	MSAMPNEXT, 'l'},
	{"\t'%c'  - reset counters to boot time values\n",	MRESET, 'l'},
	{"\t'%c'  - pause button to freeze current sample (toggle)\n",
								MPAUSE, 'l'},
	{"\n",							' ', 'l'},
	{"\t'%c'  - limit lines for per-cpu, disk and interface resources\n",
								MSYSLIMIT, 'a'},
	{"\t'%c'  - kill a process (i.e. send a signal)\n",	MKILLPROC, 'a'},
	{"\n",							' '},
	{"\t'%c'  - version information\n",			MVERSION, 'a'},
	{"\t'%c'  - help information\n",			MHELP1, 'a'},
	{"\t'%c'  - help information\n",			MHELP2, 'a'},
	{"\t'%c'  - quit this program\n",			MQUIT, 'a'},
};

static int helplines = sizeof(helptext)/sizeof(struct helptext);

static void
showhelp(int helpline)
{
	int	winlines = LINES-helpline, shown, tobeshown=1, i, lines;
	WINDOW	*helpwin;

	/*
	** create a new window for the help-info in which scrolling is
	** allowed
	*/
	helpwin = newwin(winlines, COLS, helpline, 0);
	scrollok(helpwin, 1);

	/*
	** show help-lines 
	*/
	for (i=0, lines=0, shown=0; i < helplines; i++)
	{
		if (rawreadflag)	// implies twin mode
		{
			if (helptext[i].mode == 'l')
				continue;
		}
		else
		{
			if (helptext[i].mode == 'r')
				continue;
		}

		wprintw(helpwin, helptext[i].helpline, helptext[i].helparg);

		/*
		** when the window is full, start paging interactively
		*/
		if (lines >= winlines-2 && shown >= tobeshown)
		{
			int inputkey;

			wmove(helpwin, winlines-1, 0);
			wclrtoeol(helpwin);
		      	wprintw(helpwin, "Press q (leave help), " 
					"space (next page), "
					"Enter (next line) or select key...");

			keypad(helpwin, 1);	// recognize keypad keys

			switch (inputkey = wgetch(helpwin))
			{
			   case KEY_NPAGE:
			   case KEY_PPAGE:
			   case KEY_UP:
			   case KEY_DOWN:
			   case KEY_LEFT:
			   case KEY_RIGHT:
				break;		// ignore keypad keys

			   case 'q':
				delwin(helpwin);
				return;

			   case ' ':
				shown = 0;
				tobeshown = winlines-1;
				break;

			   case '\n':
				shown = 0;
				tobeshown = 1;
				break;

			   default:
                		ungetch(inputkey);
				keywaiting = 1;
				delwin(helpwin);
				return;
			}

			wmove(helpwin, winlines-1, 0);
		}

		lines++;
		shown++;
	}

	wmove(helpwin, winlines-1, 0);
	wclrtoeol(helpwin);
	wprintw(helpwin, "End of help - press 'q' to leave help...");
        while (wgetch(helpwin) != 'q');
	delwin(helpwin);
}

/*
** function to be called to print error-messages
*/
void
generic_error(const char *format, ...)
{
	va_list	args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end  (args);
}

/*
** function to be called when the program stops
*/
void
generic_end(void)
{
	endwin();
}

/*
** function to be called when usage-info is required
*/
void
generic_usage(void)
{
	printf("\t  -%c  show fixed number of lines with system statistics\n",
			MSYSFIXED);
	printf("\t  -%c  suppress sorting of system resources\n",
			MSYSNOSORT);
	printf("\t  -%c  suppress terminated processes in output\n",
			MSUPEXITS);
	printf("\t  -%c  show limited number of lines for certain resources\n",
			MSYSLIMIT);
	printf("\t  -%c  show threads within process\n", MTHREAD);
	printf("\t  -%c  sort threads (when combined with '%c')\n", MTHRSORT, MTHREAD);
	printf("\t  -%c  show average-per-second i.s.o. total values\n\n",
			MAVGVAL);
	printf("\t  -%c  no colors in case of high occupation\n",
			MCOLORS);
	printf("\t  -%c  show general process-info (default)\n",
			MPROCGEN);
	printf("\t  -%c  show memory-related process-info\n",
			MPROCMEM);
	printf("\t  -%c  show disk-related process-info\n",
			MPROCDSK);
	printf("\t  -%c  show network-related process-info\n",
			MPROCNET);
	printf("\t  -%c  show scheduling-related process-info\n",
			MPROCSCH);
	printf("\t  -%c  show various process-info (ppid, user/group, "
	                 "date/time)\n", MPROCVAR);
	printf("\t  -%c  show command line per process\n",
			MPROCARG);
	printf("\t  -%c  show own defined process-info\n",
			MPROCOWN);
	printf("\t  -%c  show cumulated process-info per user\n",
			MCUMUSER);
	printf("\t  -%c  show cumulated process-info per program "
	                "(i.e. same name)\n",
			MCUMPROC);
	printf("\t  -%c  show cumulated process-info per container/pod\n\n",
			MCUMCONT);
	printf("\t  -%c  sort processes in order of cpu consumption "
	                "(default)\n",
			MSORTCPU);
	printf("\t  -%c  sort processes in order of memory consumption\n",
			MSORTMEM);
	printf("\t  -%c  sort processes in order of disk activity\n",
			MSORTDSK);
	printf("\t  -%c  sort processes in order of network activity\n",
			MSORTNET);
	printf("\t  -%c  sort processes in order of GPU activity\n",
			MSORTGPU);
	printf("\t  -%c  sort processes in order of most active resource "
                        "(auto mode)\n",
			MSORTAUTO);
}

/*
** functions to handle a particular tag in the /etc/atoprc and .atoprc file
*/
void
do_username(char *name, char *val)
{
	struct passwd	*pwd;

	safe_strcpy(procsel.username, val, sizeof procsel.username);

	if (procsel.username[0])
	{
		regex_t		userregex;
		int		u = 0;

		if (regcomp(&userregex, procsel.username, REG_NOSUB))
		{
			fprintf(stderr,
				"atoprc - %s: invalid regular expression %s\n",
				name, val);
			exit(1);
		}

		while ( (pwd = getpwent()))
		{
			if (regexec(&userregex, pwd->pw_name, 0, NULL, 0))
				continue;

			if (u < MAXUSERSEL-1)
			{
				procsel.userid[u] = pwd->pw_uid;
				u++;
			}
		}
		endpwent();

		procsel.userid[u] = USERSTUB;

		if (u == 0)
		{
			/*
			** possibly a numerical value has been specified
			*/
			if (numeric(procsel.username))
			{
			     procsel.userid[0] = atoi(procsel.username);
			     procsel.userid[1] = USERSTUB;
			}
			else
			{
				fprintf(stderr,
			       		"atoprc - %s: user-names matching %s "
                                        "do not exist\n", name, val);
				exit(1);
			}
		}
	}
	else
	{
		procsel.userid[0] = USERSTUB;
	}
}

void
do_procname(char *name, char *val)
{
	safe_strcpy(procsel.progname, val, sizeof procsel.progname);
	procsel.prognamesz = strlen(procsel.progname);

	if (procsel.prognamesz)
	{
		if (regcomp(&procsel.progregex, procsel.progname, REG_NOSUB))
		{
			fprintf(stderr,
				"atoprc - %s: invalid regular expression %s\n",
				name, val);
			exit(1);
		}
	}
}

void
do_maxcpu(char *name, char *val)
{
	maxcpulines = get_posval(name, val);
}

void
do_maxgpu(char *name, char *val)
{
	maxgpulines = get_posval(name, val);
}

void
do_maxdisk(char *name, char *val)
{
	maxdsklines = get_posval(name, val);
}

void
do_maxmdd(char *name, char *val)
{
	maxmddlines = get_posval(name, val);
}

void
do_maxlvm(char *name, char *val)
{
	maxlvmlines = get_posval(name, val);
}

void
do_maxintf(char *name, char *val)
{
	maxintlines = get_posval(name, val);
}

void
do_maxifb(char *name, char *val)
{
	maxifblines = get_posval(name, val);
}

void
do_maxnfsm(char *name, char *val)
{
	maxnfslines = get_posval(name, val);
}

void
do_maxcont(char *name, char *val)
{
	maxcontlines = get_posval(name, val);
}

void
do_maxnuma(char *name, char *val)
{
	maxnumalines = get_posval(name, val);
}

void
do_maxllc(char *name, char *val)
{
	maxllclines = get_posval(name, val);
}

struct colmap {
	char 	*colname;
	short	colval;
} colormap[] = {
	{ "red",	COLOR_RED,	},
	{ "green",	COLOR_GREEN,	},
	{ "yellow",	COLOR_YELLOW,	},
	{ "blue",	COLOR_BLUE,	},
	{ "magenta",	COLOR_MAGENTA,	},
	{ "cyan",	COLOR_CYAN,	},
	{ "black",	COLOR_BLACK,	},
	{ "white",	COLOR_WHITE,	},
};

static short
modify_color(char *colorname)
{
	int i;

	for (i=0; i < sizeof colormap/sizeof colormap[0]; i++)
	{
		if ( strcmp(colorname, colormap[i].colname) == 0)
			return colormap[i].colval;
	}

	// required color not found
	fprintf(stderr, "atoprc - invalid color used: %s\n", colorname);
	fprintf(stderr, "supported colors:");
	for (i=0; i < sizeof colormap/sizeof colormap[0]; i++)
		fprintf(stderr, " %s", colormap[i].colname);
	fprintf(stderr, "\n");

	exit(1);
}

void
do_colinfo(char *name, char *val)
{
	colorinfo = modify_color(val);
}

void
do_colalmost(char *name, char *val)
{
	coloralmost = modify_color(val);
}

void
do_colcrit(char *name, char *val)
{
	colorcrit = modify_color(val);
}

void
do_colthread(char *name, char *val)
{
	colorthread = modify_color(val);
}

void
do_twindir(char *name, char *val)
{
	safe_strcpy(twindir, val, RAWNAMESZ);
}

void
do_flags(char *name, char *val)
{
	int	 	i;
	extern char	twinmodeflag;

	for (i=0; val[i]; i++)
	{
		switch (val[i])
		{
		   case '-':
			break;

		   case MBARGRAPH:
			displaymode = 'D';
			break;

		   case MBARMONO:
			barmono = 1;
			break;

		   case MSORTCPU:
			showorder = MSORTCPU;
			break;

		   case MSORTGPU:
			showorder = MSORTGPU;
			break;

		   case MSORTMEM:
			showorder = MSORTMEM;
			break;

		   case MSORTDSK:
			showorder = MSORTDSK;
			break;

		   case MSORTNET:
			showorder = MSORTNET;
			break;

		   case MSORTAUTO:
			showorder = MSORTAUTO;
			break;

		   case MPROCGEN:
			showtype  = MPROCGEN;
			showorder = MSORTCPU;
			break;

		   case MPROCGPU:
			showtype  = MPROCGPU;
			showorder = MSORTGPU;
			break;

		   case MPROCMEM:
			showtype  = MPROCMEM;
			showorder = MSORTMEM;
			break;

		   case MPROCDSK:
			showtype  = MPROCDSK;
			showorder = MSORTDSK;
			break;

		   case MPROCNET:
			showtype  = MPROCNET;
			showorder = MSORTNET;
			break;

		   case MPROCVAR:
			showtype  = MPROCVAR;
			break;

		   case MPROCSCH:
			showtype  = MPROCSCH;
			showorder = MSORTCPU;
			break;

		   case MPROCARG:
			showtype  = MPROCARG;
			break;

		   case MPROCOWN:
			showtype  = MPROCOWN;
			break;

		   case MCUMUSER:
			showtype  = MCUMUSER;
			break;

		   case MCUMPROC:
			showtype  = MCUMPROC;
			break;

		   case MCUMCONT:
			showtype  = MCUMCONT;
			break;

		   case MCGROUPS:
			showtype  = MCGROUPS;
			break;

		   case MALLACTIVE:
			deviatonly = 0;
			break;

		   case MAVGVAL:
			avgval=1;
			break;

		   case MSYSFIXED:
			fixedhead = 1;
			break;

		   case MSYSNOSORT:
			sysnosort = 1;
			break;

		   case MTHREAD:
			threadview = 1;
			break;

		   case MTHRSORT:
			threadsort = 1;
			break;

		   case MCOLORS:
			usecolors = 0;
			break;

		   case MCALCPSS:
			calcpss = 1;
			break;

		   case MGETWCHAN:
			getwchan = 1;
			break;

		   case MSUPEXITS:
			suppressexit = 1;
			break;

		   case '2':	// cgroup level
		   case '3':
		   case '4':
		   case '5':
		   case '6':
		   case '7':
		   case '8':
		   case '9':
			cgroupdepth = val[i] - 0x30;
			break;

		   case 't':
			twinmodeflag++;
			break;

		   case 'I':
			idnamesuppress++;
			break;
		}
	}
}

static void
getsigwinch(int signr)
{
	winchange = 1;
}
