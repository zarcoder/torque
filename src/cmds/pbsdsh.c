#include "license_pbs.h" /* See here for the software license */
/*
 * pbs_dsh - a distribute task program using the Task Management API
 * Note that under particularly high workloads, the pbsdsh command may not function properly.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <string.h>
#include <signal.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "tm.h"
#include "mcom.h"
#include "../lib/Libifl/lib_ifl.h" /* DIS_tcp_setup, DIS_tcp_cleanup */

extern int *tm_conn;
extern int event_count;

#ifndef PBS_MAXNODENAME
#define PBS_MAXNODENAME 80
#endif
#define RESCSTRLEN (PBS_MAXNODENAME+200)

/*
 * a bit of code to map a tm_ error number to the symbol
 */

struct tm_errcode
  {
  int trc_code;
  const char   *trc_name;
  } tm_errcode[] =

  {
  { TM_ESYSTEM,         "TM_ESYSTEM" },
  { TM_ENOEVENT,        "TM_ENOEVENT" },
  { TM_ENOTCONNECTED,   "TM_ENOTCONNECTED" },
  { TM_EUNKNOWNCMD,     "TM_EUNKNOWNCMD" },
  { TM_ENOTIMPLEMENTED, "TM_ENOTIMPLEMENTED" },
  { TM_EBADENVIRONMENT, "TM_EBADENVIRONMENT" },
  { TM_ENOTFOUND,       "TM_ENOTFOUND" },
  { TM_BADINIT,         "TM_BADINIT" },
  { TM_EPERM,           "TM_EPERM" },
  { 0,                  "?" }
  };

int            *ev;
tm_event_t     *events_spawn;
tm_event_t     *events_obit;
int             numnodes;
tm_task_id     *tid;
bool            verbose = FALSE;
sigset_t        allsigs;
char           *id;
struct tm_roots rootrot;

int stdoutfd, stdoutport;
int stderrfd, stderrport;
bool grabstdoe = FALSE;

int listener_handler_pid = -1;


const char *get_ecname(

  int rc)

  {

  struct tm_errcode *p;

  for (p = &tm_errcode[0];p->trc_code;++p)
    {
    if (p->trc_code == rc)
      break;
    }

  return(p->trc_name);
  }

int fire_phasers = 0;

void bailout(

  int sig)

  {
  fire_phasers = sig;

  return;
  }



/*
 * obit_submit - submit an OBIT request
 * FIXME: do we need to retry this multiple times?
 */

int obit_submit(

  int c)     /* the task index number */

  {
  int rc;

  if (verbose)
    {
    fprintf(stderr, "%s: sending obit for task %d\n",
            id,
            c);
    }

  rc = tm_obit(*(tid + c), ev + c, events_obit + c);

  if (rc == TM_SUCCESS)
    {
    if (*(events_obit + c) == TM_NULL_EVENT)
      {
      if (verbose)
        {
        fprintf(stderr, "%s: task already dead\n", id);
        }
      }
    else if (*(events_obit + c) == TM_ERROR_EVENT)
      {
      if (verbose)
        {
        fprintf(stderr, "%s: Error on Obit return\n", id);
        }
      }
    }
  else
    {
    fprintf(stderr, "%s: failed to register for task termination notice, task %d\n",
            id,
            c);
    }

  return(rc);
  }  /* END obit_submit() */




/*
 * mom_reconnect - continually attempt to reconnect to mom
 * If we do reconnect, resubmit OBIT requests
 *
 * FIXME: there's an assumption that all tasks have already been
 * spawned and initial OBIT requests have been made.
 */

void
mom_reconnect(void)

  {
  int c, rc;

  for (;;)
    {
    tm_finalize();

    sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

    sleep(2);

    sigprocmask(SIG_BLOCK, &allsigs, NULL);

    /* attempt to reconnect */

    rc = tm_init(0, &rootrot);

    if (rc == TM_SUCCESS)
      {
      fprintf(stderr, "%s: reconnected\n",
              id);

      /* resend obit requests */

      for (c = 0;c < numnodes;++c)
        {
        if (*(events_obit + c) != TM_NULL_EVENT)
          {
          rc = obit_submit(c);

          if (rc != TM_SUCCESS)
            {
            break;  /* reconnect again */
            }
          }
        else if (verbose)
          {
          fprintf(stderr, "%s: skipping obit resend for %u\n",
                  id,
                  *(tid + c));
          }
        }

      break;
      }
    }

  return;
  }  /* END mom_reconnect() */


/*
 * wait_for_task - wait for all spawned tasks to
 * a. have the spawn acknowledged, and
 * b. the task to terminate and return the obit with the exit status
 */

int wait_for_task(

  int *nspawned) /* number of tasks spawned */

  {
  int     c;
  tm_event_t  eventpolled;
  int     nobits = 0;
  int     rc;
  int     tm_errno;

  while (*nspawned || nobits)
    {
    /* if this process was interrupted, kill the tasks */
    if (fire_phasers)
      {
      tm_event_t event;

      for (c = 0;c < numnodes;c++)
        {
        if (*(tid + c) == TM_NULL_TASK)
          continue;

        fprintf(stderr, "%s: killing task %u signal %d\n",
          id,
          *(tid + c),
          fire_phasers);

        tm_kill(*(tid + c), fire_phasers, &event);
        }

      tm_finalize();

      // kill listener handler if it has been setup
      if (listener_handler_pid != -1)
        kill(listener_handler_pid, SIGKILL);

      exit(1);
      }

    sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

    // wait for an event to complete
    rc = tm_poll(TM_NULL_EVENT, &eventpolled, 1, &tm_errno);

    sigprocmask(SIG_BLOCK, &allsigs, NULL);

    if (rc != TM_SUCCESS)
      {
      fprintf(stderr, "%s: Event poll failed, error %s\n",
        id,
        get_ecname(rc));

      if (rc == TM_ENOTCONNECTED)
        {
        mom_reconnect();
        }
      else
        {
        exit(2);
        }
      }

    if (eventpolled == TM_NULL_EVENT)
      continue;

    for (c = 0;c < numnodes;++c)
      {
      if (eventpolled == *(events_spawn + c))
        {
        /* spawn event returned - register obit */

        if (verbose)
          {
          fprintf(stderr, "%s: spawn event returned: %d (%d spawns and %d obits outstanding)\n",
                  id,
                  c,
                  *nspawned,
                  nobits);
          }

        (*nspawned)--;

        if (tm_errno)
          {
          fprintf(stderr, "%s: error %d on spawn\n",
            id,
            tm_errno);

          continue;
          }

        rc = obit_submit(c);

        if (rc == TM_SUCCESS)
          {
          if ((*(events_obit + c) != TM_NULL_EVENT) &&
              (*(events_obit + c) != TM_ERROR_EVENT))
            {
            nobits++;
            }
          }
        }
      else if (eventpolled == *(events_obit + c))
        {
        /* obit event, let's check it out */

        if (tm_errno == TM_ESYSTEM)
          {
          if (verbose)
            {
            fprintf(stderr, "%s: error TM_ESYSTEM on obit (resubmitting)\n",
              id);
            }

          sleep(2);  /* Give the world a second to take a breath */

          obit_submit(c);

          continue; /* Go poll again */
          }

        if (tm_errno != 0)
          {
          fprintf(stderr, "%s: error %d on obit for task %d\n",
            id,
            tm_errno,
            c);
          }

        /* task exited */

        if (verbose)
          {
          fprintf(stderr, "%s: obit event returned: %d (%d spawns and %d obits outstanding)\n",
                  id,
                  c,
                  *nspawned,
                  nobits);
          }

        nobits--;

        *(tid + c) = TM_NULL_TASK;

        *(events_obit + c) = TM_NULL_EVENT;

        if (verbose || (*(ev + c) != 0))
          {
          fprintf(stderr, "%s: task %d exit status %d\n",
                  id,
                  c,
                  *(ev + c));
          }
        }
      }
    }

  return PBSE_NONE;
  }  /* END wait_for_task() */


/*
 * gethostnames_from_nodefile
 *
 * Populates allnodes which is a character array of hostnames (basically numnodes * PBS_MAXHOSTNAME)
 *
 * @param allnodes - the list to populate with each hostname
 * @param nodefile - the path to the $PBS_NODEFILE
 * @return i - the total number of hosts read from $PBS_NODEFILE
 */

int gethostnames_from_nodefile(char **allnodes, char *nodefile)
  {
  FILE *fp;
  char  hostname[PBS_MAXNODENAME+2];
  int   i = 0;

  /* initialize the allnodes character array */
  *allnodes = (char *)calloc(numnodes, PBS_MAXNODENAME + 1 + sizeof(char));

  if ((fp = fopen(nodefile, "r")) == NULL)
    {
    fprintf(stderr, "failed to open %s\n", nodefile);

    exit(1);
    }

  /* read hostnames from $PBS_NODEFILE (one hostname per line) */
  while ((fgets(hostname, PBS_MAXNODENAME + 2, fp) != NULL) && (i < numnodes))
    {
    char *p;

    /* drop the trailing newline */
    if ((p = strchr(hostname, '\n')) != NULL)
      *p = '\0';

    /* copy onto the character array */
    strncpy(*allnodes + (i * PBS_MAXNODENAME), hostname, PBS_MAXNODENAME);

    i++;
    }

  fclose(fp);

  /* return count of hostnames read */
  return(i);
  }


/* return a vnode number matching targethost */
int findtargethost(char *allnodes, char *targethost)
  {
  int i;
  char *ptr;
  int vnode = 0;

  if ((ptr = strchr(targethost, '/')) != NULL)
    {
    *ptr = '\0';
    ptr++;
    vnode = atoi(ptr);
    }

  for (i = 0; i < numnodes; i++)
    {
    if (!strcmp(allnodes + (i*PBS_MAXNODENAME), targethost))
      {
      if (vnode == 0)
        return(i);

      vnode--;
      }
    else
      {
      /* Sometimes the allnodes will return the FQDN of the host
       and the PBS_NODEFILE will have the short name of the host.
       See if the shortname matches */
      std::string targetname(targethost);
      std::string the_host(allnodes + (i*PBS_MAXNODENAME));
      std::size_t dot = the_host.find_first_of(".");
      if(dot != std::string::npos)
        {
        the_host[dot] = '\0';
        std::string shortname(the_host.c_str());
        if (shortname.compare(targetname) == 0)
          {
          if (vnode == 0)
            return(i);
          vnode--;
          }
        }
      } 
    }

  if (i == numnodes)
    {
    fprintf(stderr, "%s: %s not found\n", id, targethost);
    tm_finalize();
    exit(1);
    }

  return(-1);
  }

/* prune nodelist down to a unique list by comparing with
 * the hostnames in all nodes */
int uniquehostlist(tm_node_id *nodelist, char *allnodes)
  {
  int hole, i, j, umove = 0;

  for (hole = numnodes, i = 0, j = 1; j < numnodes; i++, j++)
    {
    if (strcmp(allnodes + (i*PBS_MAXNODENAME), allnodes + (j*PBS_MAXNODENAME)) == 0)
      {
      if (!umove)
        {
        umove = 1;
        hole = j;
        }
      }
    else if (umove)
      {
      nodelist[hole++] = nodelist[j];
      }
    }

  return(hole);
  }

/**
 * Fork process to read from fd, which is assumed to be
 * ready to read. If isstdoutfd is true, fd has the
 * remote tasks' stdout, otherwise fd has the
 * remote tasks' stderr. Put the remote tasks' stdout and/or stderr
 * onto the appropriate output stream for pbsdsh.
 *
 * @see fork_listener_handler() - parent
 * @param fd file descriptor ready for reading
 * @param isstdoutfd true if fd is a stdout one, else stderr one
 */

int fork_read_handler(

  int  fd,
  bool isstdoutfd)

  {
  int  pid;
  int  bytes;
  char buf[1024];

  // check if this is the parent or if there was an error
  if ((pid = fork()) != 0)
    {
    // parent

    // return pid or -1 if failure 
    return(pid);
    }

  // child

  // keep reading from accepted connection
  while ((bytes = read_blocking_socket(fd, (void *)buf, (ssize_t)(sizeof(buf)-1))) > 0)
    {
    // terminate the buf string and output to appropriate stream
    buf[bytes] = '\0';
    fprintf(isstdoutfd ? stdout : stderr, "%s", buf);
    }

  // done reading so close the socket and exit

  close(fd);

  exit(EXIT_SUCCESS);
  }

/**
 * Wait for activity on stdoutfd and stderrfd (the listening sockets of expected stdout and stderr
 * of the spawned tasks) and dispatch handler for each as needed. Parent
 * returns pid of created child. Child continues to run until killed by caller.
 *
 * Note: stdoutfd and stderrfd area assumed to be non-blocking sockets.
 *
 * @see main() - parent
 * @see fork_read_handler() - child
 * @param stdoutfd stdout file descriptor
 * @param stderrfd stderr file descriptor
 */

int fork_listener_handler(

  int stdoutfd,
  int stderrfd)

  {
  int            pid;
  int            rfd;
  siginfo_t      info;

  // check if this is the parent or if there was an error
  if ((pid = fork()) != 0)
    {
    // parent

    // return pid or -1 if failure 
    return(pid);
    }

  // child

  // even though this is a daemon, do not close stdout and stderr since they must be left
  // open (needed in fork_read_handler())

  // keep watching the stdout/stderr fds
  //  note that they are non-blocking so accept() should not block
  while(TRUE)
    {
    // see if stdoutfd is ready
    if ((rfd = accept(stdoutfd, NULL, NULL)) != -1)
      fork_read_handler(rfd, TRUE);

    // see if stderrfd is ready
    if ((rfd = accept(stderrfd, NULL, NULL)) != -1)
      fork_read_handler(rfd, FALSE);

    // give any exiting read handlers attention so that they can actually exit
    waitid(P_ALL, 0, &info, WEXITED|WNOHANG);

    // sleep for just a short bit
    usleep(10000);
    }

    // should never get here
    exit(EXIT_FAILURE);
  }

/**
 * Create listening socket. Return port number in *port.
 *
 * @see main() - parent
 * @param *port port number assigned
 * @return listening non-blocking socket file descriptor
 */

int build_listener(

  int *port)

  {
  int s;

  struct sockaddr_in addr;
  torque_socklen_t len = sizeof(addr);

  // create the socket
  if ((s = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0)) < 0)
    {
    return(-1);
    }
  else
    {
    if (listen(s, 1024) < 0)
      {
      close(s);
      return(-1);
      }
    else
      {
      // get the port of the new socket
      if (getsockname(s, (struct sockaddr *)&addr, &len) < 0)
        {
        close(s);
        return(-1);
        }
      else
        *port = ntohs(addr.sin_port);
      }
    }

  // return the socket fd
  return (s);
  }




int main(

  int   argc,
  char *argv[])

  {
  int c;
  bool err = FALSE;
  int ncopies = -1;
  int onenode = -1;
  int rc;

  int  nspawned = 0;
  tm_node_id *nodelist = NULL;
  int start;
  int stop;
  bool sync = FALSE;

  bool pernode = FALSE;
  char *targethost = NULL;
  char *allnodes;

  struct sigaction act;

  char **ioenv;

  extern int   optind;
  extern char *optarg;

  int posixly_correct_set_by_caller = 0;
  char *envstr;

  id = (char *)calloc(60, sizeof(char));

  if (id == NULL)
    {
    fprintf(stderr, "%s: calloc failed, (%d)\n",
      id,
      errno);

    return(1);
    }

  sprintf(id, "pbsdsh(%s)",
          ((getenv("PBSDEBUG") != NULL) && (getenv("PBS_TASKNUM") != NULL))
          ? getenv("PBS_TASKNUM")
          : "");

#ifdef __GNUC__
  /* If it's already set, we won't unset it later */

  if (getenv("POSIXLY_CORRECT") != NULL)
    posixly_correct_set_by_caller = 1;

  envstr = strdup("POSIXLY_CORRECT=1");

  putenv(envstr);

#endif

  while ((c = getopt(argc, argv, "c:n:h:osuv")) != EOF)
    {
    switch (c)
      {

      case 'c':

        ncopies = atoi(optarg);

        if (ncopies <= 0)
          {
          err = TRUE;
          }

        break;

      case 'h':

        targethost = strdup(optarg); /* run on this 1 hostname */

        break;

      case 'n':

        onenode = atoi(optarg);

        if (onenode < 0)
          {
          err = TRUE;
          }

        break;

      case 'o':

        // redirect tasks' stdout and stderr to this proc's stdout and sterr streams
        // instead of job's streams
        grabstdoe = TRUE;

        break;

      case 's':

        sync = TRUE; /* force synchronous spawns */

        break;

      case 'u':

        pernode = TRUE; /* run once per node (unique hostnames) */

        break;

      case 'v':

        verbose = TRUE; /* turn on verbose output */

        break;

      default:

        err = TRUE;

        break;
      }  /* END switch (c) */

    }    /* END while ((c = getopt()) != EOF) */

  if (err || ((onenode >= 0) && (ncopies >= 1)))
    {
    fprintf(stderr, "Usage: %s [-c copies][-o][-s][-u][-v] program [args]...]\n",
      argv[0]);

    fprintf(stderr, "       %s [-n nodenumber][-o][-s][-u][-v] program [args]...\n",
      argv[0]);

    fprintf(stderr, "       %s [-h hostname][-o][-v] program [args]...\n",
      argv[0]);

    fprintf(stderr, "Where -c copies =  run  copy of \"args\" on the first \"copies\" nodes,\n");
    fprintf(stderr, "      -n nodenumber = run a copy of \"args\" on the \"nodenumber\"-th node,\n");
    fprintf(stderr, "      -o = direct tasks' stdout and stderr to the corresponding streams of pbsdsh\n");
    fprintf(stderr, "           Otherwise, tasks' stdout and/or stderr go to the job,\n");
    fprintf(stderr, "      -s = forces synchronous execution,\n");
    fprintf(stderr, "      -u = run on unique hostnames,\n");
    fprintf(stderr, "      -h = run on this specific hostname,\n");
    fprintf(stderr, "      -v = forces verbose output.\n");

    exit(1);
    }

#ifdef __GNUC__
  if (!posixly_correct_set_by_caller)
    {
    putenv((char *)"POSIXLY_CORRECT");
    free(envstr);
    }

#endif


  if (getenv("PBS_ENVIRONMENT") == NULL)
    {
    fprintf(stderr, "%s: not executing under PBS\n",
      id);

    return(1);
    }


  /*
   * Set up interface to the Task Manager
   */

  if ((rc = tm_init(0, &rootrot)) != TM_SUCCESS)
    {
    fprintf(stderr, "%s: tm_init failed, rc = %s (%d)\n",
      id,
      get_ecname(rc),
      rc);

    return(1);
    }

  sigemptyset(&allsigs);

  sigaddset(&allsigs, SIGHUP);
  sigaddset(&allsigs, SIGINT);
  sigaddset(&allsigs, SIGTERM);

  act.sa_mask = allsigs;
  act.sa_flags = 0;

  /* We want to abort system calls and call a function. */

#ifdef SA_INTERRUPT
  act.sa_flags |= SA_INTERRUPT;
#endif
  act.sa_handler = bailout;
  sigaction(SIGHUP, &act, NULL);
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);

#ifdef DEBUG

  if (rootrot.tm_parent == TM_NULL_TASK)
    {
    fprintf(stderr, "%s: I am the mother of all tasks\n",
      id);
    }
  else
    {
    fprintf(stderr, "%s: I am but a child in the scheme of things\n",
      id);
    }

#endif /* DEBUG */

  if ((rc = tm_nodeinfo(&nodelist, &numnodes)) != TM_SUCCESS)
    {
   fprintf(stderr, "%s: tm_nodeinfo failed, rc = %s (%d) nodelist= %d numnodes= %d\n",
      id,
      get_ecname(rc),
      rc,
      (nodelist==NULL) ? -1 : *nodelist,
      numnodes);

    return(1);
    }

  /* nifty unique/hostname code */
  if (pernode || targethost)
    {
    char *nodefilename;
    int   hostname_count;

    /* get node filename from PBS_NODEFILE environment variable */
    if ((nodefilename = getenv("PBS_NODEFILE")) == NULL)
      {
      fprintf(stderr, "PBS_NODEFILE environment variable not set\n");

      return(1);
      }

    /* load allnodes with the hostnames in nodefilename */
    if ((hostname_count = gethostnames_from_nodefile(&allnodes, nodefilename)) != numnodes)
      {
      fprintf(stderr, "number of hostnames (%d) read from %s does not match number of nodes (%d) in job\n",
        hostname_count, nodefilename, numnodes);

      return(1);
      }

    if (targethost)
      {
      onenode = findtargethost(allnodes, targethost);
      }
    else
      {
      numnodes = uniquehostlist(nodelist, allnodes);
      }

    free(allnodes);

    if (targethost)
      free(targethost);
    }

  /* We already checked the lower bounds in the argument processing,
     now we check the upper bounds */

  if ((onenode >= numnodes) || (ncopies > numnodes))
    {
    fprintf(stderr, "%s: only %d nodes available\n",
      id,
      numnodes);

    return(1);
    }

  /* calloc space for various arrays based on number of nodes/tasks */

  tid = (tm_task_id *)calloc(numnodes, sizeof(tm_task_id));

  events_spawn = (tm_event_t *)calloc(numnodes, sizeof(tm_event_t));

  events_obit  = (tm_event_t *)calloc(numnodes, sizeof(tm_event_t));

  ev = (int *)calloc(numnodes, sizeof(int));

  if ((tid == NULL) ||
      (events_spawn == NULL) ||
      (events_obit == NULL) ||
      (ev == NULL))
    {
    /* FAILURE - cannot alloc memory */

    fprintf(stderr, "%s: memory alloc of task ids failed\n",
      id);

    return(1);
    }

  for (c = 0;c < numnodes;c++)
    {
    *(tid + c)          = TM_NULL_TASK;
    *(events_spawn + c) = TM_NULL_EVENT;
    *(events_obit  + c) = TM_NULL_EVENT;
    *(ev + c)           = 0;
    }  /* END for (c) */

  /* Now spawn the program to where it goes */

  if (onenode >= 0)
    {
    /* Spawning one copy onto logical node "onenode" */

    start = onenode;
    stop  = onenode + 1;
    }
  else if (ncopies >= 0)
    {
    /* Spawn a copy of the program to the first "ncopies" nodes */

    start = 0;
    stop  = ncopies;
    }
  else
    {
    /* Spawn a copy on all nodes */

    start = 0;
    stop  = numnodes;
    }

  // allocate space for 3 pointers in task environment: 1 each for stdout and sterr
  // if needed and a null one to terminate the list
  if ((ioenv = (char **)calloc(3, sizeof(char **))) == NULL)
    {
    /* FAILURE - cannot alloc memory */

    fprintf(stderr,"%s: memory alloc of ioenv failed\n",
      id);

    return(1);
    }

  if (grabstdoe)
    {
    // get listening sockets for tasks' stdout and stderr
    if ((stdoutfd = build_listener(&stdoutport)) == -1)
      {
      fprintf(stderr,"%s: build_listener() failed for stdout\n", id);
      return(1);
      }

    if ((stderrfd = build_listener(&stderrport)) == -1)
      {
      fprintf(stderr,"%s: build_listener() failed for stderr\n", id);
      return(1);
      }

    // set up listener handler to receive incoming requests and redirect
    // tasks' stdout and stderr to this proc's stdout and stderr streams
    if ((listener_handler_pid = fork_listener_handler(stdoutfd, stderrfd)) < 0)
      {
      fprintf(stderr, "%s: fork_listener_handler() failed with %d(%s)\n", id, errno, strerror(errno));
      return(1);
      }

    // put the stdout listening port into task environment string
    if ((*ioenv = (char *)calloc(50, sizeof(char))) == NULL)
      {
      /* FAILURE - cannot alloc memory */

      fprintf(stderr,"%s: memory alloc of *ioenv failed\n",
        id);

      return(1);
      }

    snprintf(*ioenv,49,"TM_STDOUT_PORT=%d", 
      stdoutport);

    // put the stderr listening port into task environment string
    if ((*(ioenv+1) = (char *)calloc(50, sizeof(char))) == NULL)
      {
      /* FAILURE - cannot alloc memory */

      fprintf(stderr,"%s: memory alloc of *(ioenv+1) failed\n",
        id);

      return(1);
      }

    snprintf(*(ioenv+1),49,"TM_STDERR_PORT=%d", 
      stderrport);
    }

  sigprocmask(SIG_BLOCK, &allsigs, NULL);

  for (c = start; c < stop; ++c)
    {
    if ((rc = tm_spawn(
                argc - optind,
                argv + optind,
                ioenv,
                *(nodelist + c),
                tid + c,
                events_spawn + c)) != TM_SUCCESS)
      {
      fprintf(stderr, "%s: spawn failed on node %d err %s\n",
        id,
        c,
        get_ecname(rc));
      }
    else
      {
      if (verbose)
        fprintf(stderr, "%s: spawned task %d\n",
          id,
          c);

      ++nspawned;

      if (sync)
        rc = wait_for_task(&nspawned); /* one at a time */
      }
    }    /* END for (c) */

  if (!sync)
    rc = wait_for_task(&nspawned); /* wait for all to finish */
  if (rc != 0)
    return rc;

  /*
   * Terminate interface with Task Manager
   */

  tm_finalize();

  // kill listener handler
  if (listener_handler_pid != -1)
    kill(listener_handler_pid, SIGKILL);

  return 0;
  }  /* END main() */

/* END pbsdsh.c */
