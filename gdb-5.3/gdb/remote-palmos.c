/* Remote target communications for serial-line targets in custom GDB protocol
   Copyright 1988, 1991, 1992, 1993, 1994, 1995, 1996 Free Software Foundation, Inc.
   
   Written by Kenneth Albanowski <kjahds@kjahds.com>, derived (with
   assistance from Palm Computing, Inc.) from work by
   
     Kenneth Albanowski,
     Donald Jeff Dionne <jeff@RyeHam.ee.ryerson.ca>,
     Kresten Krab Thorup <krab@daimi.aau.dk>,
     and whoever wrote remote.c and xmodem.c.

This file is part of GDB.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "gdb_string.h"
#include <fcntl.h>
#include "frame.h"
#include "inferior.h"
#include "bfd.h"
#include "symfile.h"
#include "target.h"
#include "gdb_wait.h"
/*#include "terminal.h"*/
#include "gdbcmd.h"
#include "objfiles.h"
#include "gdb-stabs.h"
#include "gdbthread.h"

#ifdef USG
#include <sys/types.h>
#endif

#include <signal.h>
#include "serial.h"
#include "xmodem.h"

/* Prototypes for local functions */

static int remote_write_bytes PARAMS ((CORE_ADDR memaddr,
				       char *myaddr, int len));

static int remote_read_bytes PARAMS ((CORE_ADDR memaddr,
				      char *myaddr, int len));

static void remote_files_info PARAMS ((struct target_ops *ignore));

static int remote_xfer_memory PARAMS ((CORE_ADDR memaddr, char *myaddr,
				       int len, int should_write,
				       struct mem_attrib *attrib,
				       struct target_ops *target));

static void remote_prepare_to_store PARAMS ((void));

static void remote_fetch_registers PARAMS ((int regno));

static void remote_resume PARAMS ((ptid_t ptid, int step,
				   enum target_signal siggnal));

static int remote_start_remote PARAMS ((PTR dummy));

static void remote_open PARAMS ((char *name, int from_tty));

static void remote_open_pilot PARAMS ((char *name, int from_tty));

static void extended_remote_open PARAMS ((char *name, int from_tty));

static void remote_open_1 PARAMS ((char *, int, struct target_ops *));

static void remote_close PARAMS ((int quitting));

static void remote_store_registers PARAMS ((int regno));

static void remote_mourn PARAMS ((void));

static void remote_mourn_pilot PARAMS ((void));

static void extended_remote_restart PARAMS ((void));

static void extended_remote_mourn PARAMS ((void));

static void extended_remote_create_inferior PARAMS ((char *, char *, char **));

static void remote_mourn_1 PARAMS ((struct target_ops *));

static int getpkt PARAMS ((char **buf, int forever));

static int putpkt PARAMS ((char *buf, int len));

static void remote_send PARAMS ((char *buf));

static int readchar PARAMS ((int timeout));

static ptid_t remote_wait PARAMS ((ptid_t ptid,
				   struct target_waitstatus *status));

static void remote_kill PARAMS ((void));

static int tohex PARAMS ((int nib));

static int fromhex PARAMS ((int a));

static void remote_detach PARAMS ((char *args, int from_tty));

static void remote_interrupt PARAMS ((int signo));

static void remote_interrupt_twice PARAMS ((int signo));

static void interrupt_query PARAMS ((void));

static void remote_insert_wbreakpoint PARAMS ((CORE_ADDR addr,
					       enum target_signal signo));

static enum target_signal remote_remove_wbreakpoint PARAMS ((void));

extern struct target_ops palmos_ops, pilot_ops;	/* Forward decl */

static CORE_ADDR text_addr=0, data_addr=0, bss_addr=0;
static CORE_ADDR save_ssp, save_usp;
static enum target_signal wbreakpoint_signo;
static CORE_ADDR wbreakpoint_addr;
/*static struct Pilot_state state;*/

#define MAX_BREAKS 5

static struct {
	unsigned long address;
	int on;
} breakpoint[MAX_BREAKS + 1];

static char regs[16*4 + 8 + 8*12 + 3*4];

/* Portable memory access macros */

#define get_long(ptr) ((((unsigned char*)(ptr))[0] << 24) | \
                       (((unsigned char*)(ptr))[1] << 16) | \
                       (((unsigned char*)(ptr))[2] << 8)  | \
                       (((unsigned char*)(ptr))[3] << 0))

#define get_treble(ptr) ((((unsigned char*)(ptr))[0] << 16) | \
                         (((unsigned char*)(ptr))[1] << 8)  | \
                         (((unsigned char*)(ptr))[2] << 0))

#define get_short(ptr) ((((unsigned char*)(ptr))[0] << 8)  | \
                        (((unsigned char*)(ptr))[1] << 0))

#define get_byte(ptr) (((unsigned char*)(ptr))[0])

#define set_long(ptr,val) ((((unsigned char*)(ptr))[0] = ((val) >> 24) & 0xff), \
                          (((unsigned char*)(ptr))[1] = ((val) >> 16) & 0xff), \
                          (((unsigned char*)(ptr))[2] = ((val) >> 8) & 0xff), \
                          (((unsigned char*)(ptr))[3] = ((val) >> 0) & 0xff))

#define set_treble(ptr,val) ((((unsigned char*)(ptr))[0] = ((val) >> 16) & 0xff), \
                             (((unsigned char*)(ptr))[1] = ((val) >> 8) & 0xff), \
                             (((unsigned char*)(ptr))[2] = ((val) >> 0) & 0xff))

#define set_short(ptr,val) ((((unsigned char*)(ptr))[0] = ((val) >> 8) & 0xff), \
                            (((unsigned char*)(ptr))[1] = ((val) >> 0) & 0xff))

#define set_byte(ptr,val) (((unsigned char*)(ptr))[0]=(val))

/* Following CRC code borrowed from xmodem.c */

#define CRC16 0x1021		/* Generator polynomial (X^16 + X^12 + X^5 + 1) */

static unsigned short *crctab;

/* Call this to init the fast CRC-16 calculation table.  */

static void
crcinit ()
{
  static int crctab_inited = 0;
  int val;

  if (crctab_inited == 1)
    return;

  crctab = xmalloc (256 * sizeof (short));

  for (val = 0; val <= 255; val++)
    {
      int i;
      unsigned int crc;

      crc = val << 8;

      for (i = 0; i < 8; ++i)
	{
	  crc <<= 1;

	  if (crc & 0x10000)
	    crc ^= CRC16;
	}

      crctab [val] = crc;
    }

  crctab_inited = 1;
}

/* Calculate a CRC-16 for the LEN byte message pointed at by P.  */

static unsigned short
docrc (p, len)
     unsigned char *p;
     int len;
{
  unsigned short crc = 0;

  while (len-- > 0)
    crc = (crc << 8) ^ crctab [(crc >> 8) ^ *p++];

  return crc;
}

  
static void
get_offsets ()
{
  struct section_offsets *offs;
  
  if (symfile_objfile == NULL)
    return;
    
  if ((text_addr == 0) && (data_addr==0) && (bss_addr==0))
    return;

  offs = (struct section_offsets *) alloca (sizeof (struct section_offsets)
					    + symfile_objfile->num_sections
					    * sizeof (offs->offsets));
  memcpy (offs, symfile_objfile->section_offsets,
	  sizeof (struct section_offsets)
	  + symfile_objfile->num_sections
	  * sizeof (offs->offsets));

  offs->offsets[SECT_OFF_TEXT (symfile_objfile)] = text_addr;

  /* This is a temporary kludge to force data and bss to use the same offsets
     because that's what nlmconv does now.  The real solution requires changes
     to the stub and remote.c that I don't have time to do right now.  */

  offs->offsets[SECT_OFF_DATA (symfile_objfile)] = data_addr;
  offs->offsets[SECT_OFF_BSS  (symfile_objfile)] = data_addr;

  objfile_relocate (symfile_objfile, offs);
}

/* Stub for catch_errors.  */

static int
remote_start_remote (dummy)
     PTR dummy;
{
     struct target_waitstatus status;
  immediate_quit = 1;		/* Allow user to interrupt it */
  
#if 0
  remote_wait(0,&status); /* Wait for remote to halt, hopefully fetching
                             offsets in the process */

  get_offsets ();		/* Get text, data & bss offsets */
#endif

  immediate_quit = 0;

  start_remote ();		/* Initialize gdb process mechanisms */
  return 1;
}

/* Open a connection to a remote debugger.
   NAME is the filename used for communication.  */

static void
remote_open (name, from_tty)
     char *name;
     int from_tty;
{
  remote_open_1 (name, from_tty, &palmos_ops);
}

static void
remote_open_pilot (name, from_tty)
     char *name;
     int from_tty;
{
  remote_open_1 (name, from_tty, &pilot_ops);
}

/* Generic code for opening a connection to a remote target.  */

static struct serial *remote_desc = NULL;
static int startup = 1;

static void
remote_open_1 (name, from_tty, target)
     char *name;
     int from_tty;
     struct target_ops *target;
{
  if (name == 0)
    name = "localhost:2000";  /* Connect to emulator by default.  */

  target_preopen (from_tty);

  unpush_target (target);

  remote_desc = serial_open(name);
  if (!remote_desc)
    perror_with_name(name);
    
  if (baud_rate == -1)
    baud_rate = 57600;

  if (baud_rate != -1)
    {
    if (serial_setbaudrate (remote_desc, baud_rate))
       {
       serial_close (remote_desc);
       perror_with_name (name);
      }
  }
  
  serial_raw (remote_desc);
  
  crcinit();
  
  if (from_tty)
    {
      puts_filtered ("Remote debugging under PalmOS using ");
      puts_filtered (name);
      puts_filtered ("\n");
      startup = 1;
    }
  push_target (target);	/* Switch to using remote target now */

  inferior_ptid = pid_to_ptid (42000);
  /* Start the remote connection; if error (0), discard this target.
     In particular, if the user quits, be sure to discard it
     (we'd be in an inconsistent state otherwise).  */
  if (!catch_errors (remote_start_remote, (char *)0, 
		     "Couldn't establish connection to remote target\n", RETURN_MASK_ALL))
    pop_target();
}

/* Clean up connection to a remote debugger.  */

/* ARGSUSED */
static void
remote_close (quitting)
     int quitting;
{
  if (remote_desc)
    serial_close(remote_desc);
  remote_desc = NULL;
}

/* This takes a program previously attached to and detaches it.  After
   this is done, GDB can be used to debug some other program.  We
   better not have left any breakpoints in the target program or it'll
   die when it hits one.  */

static void
remote_detach (args, from_tty)
     char *args;
     int from_tty;
{
  pop_target ();

  if (from_tty)
    puts_filtered ("Ending remote debugging.\n");
}

static enum target_signal last_sent_signal = TARGET_SIGNAL_0;
int last_sent_step;

static void
remote_resume (ptid, step, siggnal)
     ptid_t ptid;
     int step;
     enum target_signal siggnal;
{
   unsigned long sr;
   unsigned short ins;
   char buffer[90];
   
   buffer[0] = 0x07;
   buffer[1] = 0;
   memcpy(buffer+2, regs+REGISTER_BYTE(0), 60);    /* D0-D7, A0-A6 */
   sr = get_long(regs+REGISTER_BYTE(16));
   
   if (step) {
     remote_read_bytes (get_long(regs+REGISTER_BYTE(17)),
				 (void *)&ins,
				 2);

     if (get_short(&ins) == 0x4e4f) {
       remote_insert_wbreakpoint(get_long(regs+REGISTER_BYTE(17)) + 4,
				 TARGET_SIGNAL_0);
       sr &= 0x7FFF;
     } else {
       sr |= 0x8000;
     }
   } else {
     sr &= 0x7FFF;
   }

   if (sr & 0x2000)
     {
       memcpy(buffer+62, &save_usp, 4);
       memcpy(buffer+66, regs+REGISTER_BYTE(15), 4); /* Store SSP */
     }
   else
     {
       memcpy(buffer+62, regs+REGISTER_BYTE(15), 4); /* Store USP */
       memcpy(buffer+66, &save_ssp, 4);
     }
   memcpy(buffer+70, regs+REGISTER_BYTE(17), 4);   /* Store PC */
   set_short(buffer+74, sr);   /* Store SR */
   
   memset(buffer+76, 0, 14); /* Zero out watch parameters */
   
   last_sent_signal = siggnal;
   last_sent_step = step;
   
   putpkt(buffer, 90);
}

static void (*ofunc)();

static void
remote_interrupt (signo)
     int signo;
{
  char buffer[10];
  signal (signo, remote_interrupt_twice);
  
  fputs_filtered ("Sending query. (Press Ctrl-C again to give up)\n", gdb_stdout);
  
  buffer[0] = 0;
  buffer[1] = 0;
  putpkt(buffer, 2);
}

static void
remote_interrupt_twice (signo)
     int signo;
{
  signal (signo, ofunc);
  
  interrupt_query ();

  signal (signo, remote_interrupt);
}

/* Ask the user what to do when an interrupt is received.  */

static void
interrupt_query ()
{
  target_terminal_ours ();

  if (query ("Interrupted while waiting for the program.\n\
Give up (and stop debugging it)? "))
    {
      target_mourn_inferior ();
      throw_exception (RETURN_QUIT);
    }

  target_terminal_inferior ();
}

static long computeSignal( long exceptionVector )
{
  long sigval;

  switch (exceptionVector) {
    case 2 : sigval = 10; break; /* bus error           */
    case 3 : sigval = 10; break; /* address error       */
    case 4 : sigval = 4;  break; /* illegal instruction */
    case 5 : sigval = 8;  break; /* zero divide         */
    case 6 : sigval = 8; break; /* chk instruction     */
    case 7 : sigval = 8; break; /* trapv instruction   */
    case 8 : sigval = 11; break; /* privilege violation */
    case 9 : sigval = 5;  break; /* trace trap          */
    case 10: sigval = 4;  break; /* line 1010 emulator  */
    case 11: sigval = 4;  break; /* line 1111 emulator  */

      /* Coprocessor protocol violation.  Using a standard MMU or FPU
	 this cannot be triggered by software.  Call it a SIGBUS.  */
    case 13: sigval = 10;  break;

    case 31: sigval = 2;  break; /* interrupt           */
    case 32: sigval = 5;  break; /* breakpoint          */

      /* This is a trap #8 instruction.  Apparently it is someone's software
	 convention for some sort of SIGFPE condition.  Whose?  How many
	 people are being screwed by having this code the way it is?
	 Is there a clean solution?  */
    case 40: sigval = 8;  break; /* floating point err  */

    case 48: sigval = 8;  break; /* floating point err  */
    case 49: sigval = 8;  break; /* floating point err  */
    case 50: sigval = 8;  break; /* zero divide         */
    case 51: sigval = 8;  break; /* underflow           */
    case 52: sigval = 8;  break; /* operand error       */
    case 53: sigval = 8;  break; /* overflow            */
    case 54: sigval = 8;  break; /* NAN                 */
    default: 
      sigval = 7;         /* "software generated"*/
  }
  return (sigval);
}

/* If nonzero, ignore the next kill.  */
extern int kill_kludge;

/* Read a single character from the remote end. */

static int
readchar (timeout)
     int timeout;
     {
       int ch;
       
       ch = serial_readchar (remote_desc, timeout);
         
       switch (ch)
         {
         case SERIAL_EOF:
          error ("Remote connection closed");
         case SERIAL_ERROR:
         perror_with_name ("Remote communication error");
       case SERIAL_TIMEOUT:
       return ch;
     default:
        return ch;
      }
}

/* Wait until the remote machine stops, then return,
   storing status in STATUS just as `wait' would.
   Returns "pid" (though it's not clear what, if anything, that
   means in the case of this target).  */

static ptid_t
remote_wait (ptid, status)
     ptid_t ptid;
     struct target_waitstatus *status;
{
  char * buf;
  int len;
  int thread_num = -1;
  unsigned long ins;
  
  if (startup) {
    fputs_filtered ("Waiting... (Press Ctrl-C to connect to halted machine)\n", gdb_stdout);
    startup = 0;
  }
  	
  status->kind = TARGET_WAITKIND_EXITED;
    status->value.integer = 0;
	    
	while (1) {
	   ofunc = (void (*)()) signal (SIGINT, remote_interrupt);
  	   len = getpkt(&buf, 1);
  	   signal (SIGINT, ofunc);
  	   
  	   if (len<10) /* Reception failed, skip */
  	     continue;
  	   
  	   if ((buf[3] != 0) || (buf[4] != 0) || (buf[5] != 0))
  	     /* Not a debugging packet, skip */
  	     continue;
  	   
	  if (buf[10] == (char)0x7F) { /* Message */
	      int i;
	      for (i=12;i<len;i++) {
	        if (buf[i] == '\r')
	          buf[i] = '\n';
	      }
	      buf[len] = 0;
	      fputs_filtered (buf+12, gdb_stdout);
	      continue;
	  }
	  else if (buf[10] == (char)0x80) { /* Break & state */
	    unsigned long sr;

#define State_resetted 12
#define State_exception 14
#define State_D0 16
#define State_D1 20
#define State_D2 24
#define State_D3 28
#define State_A0 48
#define State_USP 76
#define State_SSP 80
#define State_PC 84
#define State_SR 88
#define State_INS State_SR+30
#define Breakpoint_0 State_SR+2+30

	    
	    status->kind = TARGET_WAITKIND_STOPPED;
	    status->value.sig = computeSignal(get_short(buf+State_exception)/4);
	    
	    memcpy(regs+REGISTER_BYTE(0), buf+State_D0, 60); /* D0-D7, A0-A6 */
	    
	    sr = get_short(buf+State_SR);
	    if (sr & 0x2000) /* Check supervisor bit */
	      {
		memcpy(&save_usp, buf+State_USP, 4);
		memcpy(regs+REGISTER_BYTE(15), buf+State_SSP, 4); /* SSP */
	      }
	    else
	      {
		memcpy(regs+REGISTER_BYTE(15), buf+State_USP, 4); /* USP */
		memcpy(&save_ssp, buf+State_SSP, 4);
	      }
	    
	    set_long(regs+REGISTER_BYTE(16), sr); /* SR */
	    memcpy(regs+REGISTER_BYTE(17), buf+State_PC, 4); /* PC */
	    
	    if (get_byte(buf+State_resetted) != 0)
	      {
		/* If the target has just been reset we will ignore the
		   breakpoint state and forcibly clear all extant breakpoints,
		   thus avoiding out of date information from the target.  */
		memset(breakpoint, 0, sizeof breakpoint);
	      }
	    else
	      {
		int i;
		for (i=0;i<6;i++)
		  {
		    breakpoint[i].address = get_long(buf+Breakpoint_0+i*6);
		    breakpoint[i].on = get_byte(buf+Breakpoint_0+4+i*6);
		  }
	      }

	    if (get_long(buf+State_PC) == wbreakpoint_addr) {
	      enum target_signal truesig = remote_remove_wbreakpoint();
	      if (truesig != TARGET_SIGNAL_0)
		status->value.sig = truesig;
	    }
	    
	    /* We used to test (get_short(buf+State_exception) == 40 * 4) as
	       well, but Ton reports that the Simulator returns bogus values
	       for this field of the state packet.  */
	    if (get_long(buf+State_D3) == 0x12BEEF34) {
	      CORE_ADDR bp;
	      struct symtab_and_line sal;

#if 0
  	      puts_filtered ("Got target position.\n");
#endif
	      text_addr = get_long(buf+State_D0);
	      bss_addr =  get_long(buf+State_D1);
	      data_addr = get_long(buf+State_D2);
	      get_offsets();
	      /* Find pc value corresponding to first executable statement
	         after the PilotMain prologue and insert breakpoint there.  */
	      bp = get_long(buf+State_A0);
	      INIT_SAL (&sal);
	      sal = find_pc_line (bp, 0);
	      if (sal.pc && bp != sal.pc && bp > sal.pc && bp <= sal.end)
	        bp = sal.end;
	      remote_insert_wbreakpoint(bp, TARGET_SIGNAL_STOP);
              remote_resume(inferior_ptid,0,TARGET_SIGNAL_0);
	      stop_soon_quietly = 0;
              continue;
	    }
	    break;
	    
	  }
  	  puts_filtered ("Unknown packet received.\n");
	}
	return inferior_ptid;
}

/* Number of bytes of registers this stub implements.  */
static int register_bytes_found;

/* Read the remote registers into the block REGS.  */
/* Currently we just read all the registers, so we don't use regno.  */
/* ARGSUSED */
static void
remote_fetch_registers (regno)
     int regno;
{
  int i;
  unsigned long sr;
  
  for (i = 0; i < NUM_REGS; i++)
      supply_register (i, &regs[REGISTER_BYTE(i)]);
}

/* Prepare to store registers.  Since we may send them all (using a
   'G' request), we have to read out the ones we don't want to change
   first.  */

static void 
remote_prepare_to_store ()
{
  /* no-op, registers are automatic retrieved */
  return;
}

/* Store register REGNO, or all registers if REGNO == -1, from the contents
   of REGISTERS.  FIXME: ignores errors.  */

static void
remote_store_registers (regno)
     int regno;
{
  int i;
  for (i = 0; i < NUM_REGS; i++)
      if ((regno==-1) || (i == regno))
          memcpy(&regs[REGISTER_BYTE(i)], &registers[REGISTER_BYTE(i)], 4);
}

/* 
   Use of the data cache *used* to be disabled because it loses for looking at
   and changing hardware I/O ports and the like.  Accepting `volatile'
   would perhaps be one way to fix it.  Another idea would be to use the
   executable file for the text segment (for all SEC_CODE sections?
   For all SEC_READONLY sections?).  This has problems if you want to
   actually see what the memory contains (e.g. self-modifying code,
   clobbered memory, user downloaded the wrong thing).  

   Because it speeds so much up, it's now enabled, if you're playing
   with registers you turn it of (set remotecache 0)
*/

/* Read a word from remote address ADDR and return it.
   This goes through the data cache.  */

#if 0	/* unused? */
static int
remote_fetch_word (addr)
     CORE_ADDR addr;
{
  return dcache_fetch (remote_dcache, addr);
}

/* Write a word WORD into remote address ADDR.
   This goes through the data cache.  */

static void
remote_store_word (addr, word)
     CORE_ADDR addr;
     int word;
{
  dcache_poke (remote_dcache, addr, word);
}
#endif	/* 0 (unused?) */


/* Write memory data directly to the remote machine.
   This does not inform the data cache; the data cache uses this.
   MEMADDR is the address in the remote memory space.
   MYADDR is the address of the buffer in our space.
   LEN is the number of bytes.

   Returns number of bytes transferred, or 0 for error.  */

static int
remote_write_bytes (memaddr, myaddr, len)
     CORE_ADDR memaddr;
     char *myaddr;
     int len;
{
	char buffer[280];
	char * ret;
	int l;
	unsigned long todo, done;
	
	/* printf("wanting to write %d bytes at %d\n", len, memaddr); */
	
	done = 0;
	while (done < len) {
	  todo = (len-done);
	  if (todo > 256)
	    todo = 256;
	  
	  buffer[0] = 0x02;
	  buffer[1] = 0;
	  set_long(buffer+2, memaddr + done);
	  set_short(buffer+6, todo);
	  
	  memcpy(buffer+8, myaddr+done, todo);
	  
	  putpkt(buffer, 8 + todo);
	  
	  if (getpkt(&ret, 0) != 12) {
	    break;
	  }
	  done += todo;
	}
	/* printf("Actually wrote %d bytes\n", done); */
	return done;
}

static char *
remote_get_macsbug_name (CORE_ADDR memaddr)
{
  static const char sysPktGetRtnNameCmd = '\x04';
  static const char sysPktGetRtnNameRsp = '\x84';

  char buffer[6];
  char *pkt;

  buffer[0] = sysPktGetRtnNameCmd;
  buffer[1] = 0;
  set_long (buffer+2, memaddr);

  putpkt (buffer, sizeof buffer);

  getpkt (&pkt, 0);
  if (pkt[10] == sysPktGetRtnNameRsp)
    {
      char *name = &pkt[24];
      return (name[0] == '\0')? NULL : name;
    }
  else
    return "funky packet";
}

char *
last_chance_lookup_by_pc (CORE_ADDR pc)
{
  return remote_get_macsbug_name (pc);
}

/* Read memory data directly from the remote machine.
   This does not use the data cache; the data cache uses this.
   MEMADDR is the address in the remote memory space.
   MYADDR is the address of the buffer in our space.
   LEN is the number of bytes.

   Returns number of bytes transferred, or 0 for error.  */

static int
remote_read_bytes (memaddr, myaddr, len)
     CORE_ADDR memaddr;
     char *myaddr;
     int len;
{
	char buffer[8];
	char * ret;
	unsigned long todo, done;
	
	done = 0;
	while (done < len) {
	  todo = (len-done);
	  if (todo > 256)
	    todo = 256;
	  
	  buffer[0] = 0x01;
	  buffer[1] = 0;
	  set_long(buffer+2, memaddr + done);
	  set_short(buffer+6, todo);
	  
	  putpkt(buffer, 8);
	  
	  if (getpkt(&ret, 0) == todo+12) {
	    memcpy(myaddr+done, ret+12, todo);
	  } else {
	    break;
	  }
	  done += todo;
	}
	return done;
}

/* Read or write LEN bytes from inferior memory at MEMADDR, transferring
   to or from debugger address MYADDR.  Write to inferior if SHOULD_WRITE is
   nonzero.  Returns length of data written or read; 0 for error.  */

/* ARGSUSED */
static int
remote_xfer_memory(memaddr, myaddr, len, should_write, attrib, target)
     CORE_ADDR memaddr;
     char *myaddr;
     int len;
     int should_write;
     struct mem_attrib *attrib;			/* ignored */
     struct target_ops *target;			/* ignored */
{
  CORE_ADDR targaddr;
  int targlen;
  int (*xfer) (CORE_ADDR, char *, int) =
    should_write? remote_write_bytes : remote_read_bytes;

  REMOTE_TRANSLATE_XFER_ADDRESS (memaddr, len, &targaddr, &targlen);
  return xfer (targaddr, myaddr, targlen);
}

   
#if 0
/* Enable after 4.12.  */

void
remote_search (len, data, mask, startaddr, increment, lorange, hirange
	       addr_found, data_found)
     int len;
     char *data;
     char *mask;
     CORE_ADDR startaddr;
     int increment;
     CORE_ADDR lorange;
     CORE_ADDR hirange;
     CORE_ADDR *addr_found;
     char *data_found;
{
}
#endif /* 0 */

static void
remote_files_info (ignore)
     struct target_ops *ignore;
{
  puts_filtered ("Debugging a target over a serial line.\n");
}

static unsigned char transid = 0x11;

/* Send a packet to the remote machine.
   The data of the packet is in BUF.  */

static int
putpkt (buf, len)
     char *buf;
     int len;
{
  static unsigned char buffer[0xffff];
  int i;
  
  buffer[0] = 0xBE;
  buffer[1] = 0xEF;
  buffer[2] = 0xED;
  buffer[3] = 0;
  buffer[4] = 0;
  buffer[5] = 0;
  buffer[6] = len >> 8;
  buffer[7] = len & 0xff;
  buffer[8] = ++transid;
  buffer[9] = 0;
  for (i=0;i<9;i++)
    buffer[9] += buffer[i];
  
  memcpy(buffer+10, buf, len);
  
  set_short(buffer+len+10, docrc(buffer, len+10));
  
  /*printf("Sending: ");
  for (i=0;i<len+12;i++) {
     printf("%.2X ", buffer[i]);
  }
  printf("!\n");*/
   
  if (serial_write(remote_desc, buffer, len+12))
         perror_with_name ("putpkt: write failed");

  return 0;
}

/* Read a packet from the remote machine, with error checking,
   and store it in BUF.  BUF is expected to be of size PBUFSIZ.
   If FOREVER, wait forever rather than timing out; this is used
   while the target is executing user code.  */

static int
getpkt (buf, forever)
     char **buf;
     int forever;
{
  static unsigned char buffer[0xffff];
  int state = 0;
  int c;
  int i;
  unsigned int src, dest, type, len, csum, crc;
  
  while(1) {
    c = readchar(-1);
    if (c == SERIAL_TIMEOUT) {
       if (!forever)
          return 0;
    }
    
    buffer[state] = c;
    
    switch (state) {
    	case 0:
    	    if (c == 0xBE) state++; else state=0;
    	    break;
    	case 1:
    	    if  (c == 0xEF) state++; else state=0; 
    	    break;
    	case 2:
    	    if  (c == 0xED) state++; else state=0;
    	    break;
    	case 3:
    	case 4:
    	case 5:
    	case 6:
    	case 7:
    	case 8:
    	    state++;
    	    break;
    	case 9:
    	    csum = 0;
    	    for (i=0;i<9;i++)
    	      csum += buffer[i];
    	    if ((csum & 0xff) == c) {
    	      len = (buffer[6] << 8) | buffer[7];
    	      state++;
    	    }
    	    else
    	      state=0;
    	    break;
    	default:
    	    if (state >= 10) {
    	      if (state < 10+len) {
    	        state++;
    	      } 
    	      else if (state == 10+len) {
    	        crc = c;
    	        state++;
    	      } else if (state == 11+len) {
    	        unsigned long mycrc = docrc(buffer,len+10);
    	        crc = (crc<<8)|c;
    	        if ((crc & 0xffff) == mycrc) {
    	           *buf = (char*)buffer;
    	           return len+10;
    	        } else 
    	          state = 0;
    	      } else
    	        state = 0;
    	    } else
    	      state = 0;
       }
       
  }
}

static void
remote_kill ()
{
  char buffer[96];

  /* For some mysterious reason, wait_for_inferior calls kill instead of
     mourn after it gets TARGET_WAITKIND_SIGNALLED.  Work around it.  */
  if (kill_kludge)
    {
      kill_kludge = 0;
      target_mourn_inferior ();
      return;
    }
    
  /* Warm boot the Pilot */
  
  buffer[0] = 0x0A;
  buffer[1] = 0;
  set_short(buffer+2, 0xA08C); /* SysReboot */
  set_long(buffer+4, 0); /*D0*/
  set_long(buffer+8, 0); /*A0*/
  set_short(buffer+12, 0); /* No parameters */
  
  putpkt(buffer, 14);

  /* Don't wait for it to die.  I'm not really sure it matters whether
     we do or not.  For the existing stubs, kill is a noop.  */
  target_mourn_inferior ();
}

static void
remote_mourn ()
{
  remote_mourn_1 (&palmos_ops);
}

static void
remote_mourn_pilot ()
{
  remote_mourn_1 (&pilot_ops);
}

static void
remote_mourn_1 (struct target_ops *ops)
{
  unpush_target (ops);
  generic_mourn_inferior ();
}



/* Note: we must use native breakpoint support, as code segments are in
   write-protected memory, and thus cannot easily have breaks written
   over them. */

/* Send breakpoint structure to the Pilot. Return non-zero on error */

static int
set_breakpoints()
{
  int i;
  char buffer[90];
  char * ret;
  
  buffer[0] = 0x0c;
  buffer[1] = 0;
  
  for(i=0;i<6;i++) {
    set_long(buffer+2+i*6, breakpoint[i].address);
    set_byte(buffer+2+4+i*6, breakpoint[i].on);
    set_byte(buffer+2+5+i*6, 0);
  }
  
  putpkt(buffer, 38);
  
  if ((i = getpkt(&ret, 0))) {
    return ((unsigned char)ret[10] != (unsigned char)0x8c) || (i != 12);
  }
  return 1;
}

static int
remote_insert_breakpoint (addr, contents_cache)
     CORE_ADDR addr;
     char *contents_cache;
{
  int i;
  for (i=0;i<MAX_BREAKS;i++)
    if (breakpoint[i].on == 0)
      break;
  if (i < MAX_BREAKS) {
    breakpoint[i].address = addr;
    breakpoint[i].on = 1;
    return set_breakpoints();
  } else {
        fprintf_filtered (gdb_stderr,
        "Too many break points, break point not installed\n");
       return (1);
  }                            
}

static int
remote_remove_breakpoint (addr, contents_cache)
     CORE_ADDR addr;
     char *contents_cache;
{
  int i;
  for (i=0;i<MAX_BREAKS;i++)
    if (breakpoint[i].on && breakpoint[i].address == addr)
      break;
  if (i<MAX_BREAKS) {
    breakpoint[i].address = 0;
    breakpoint[i].on = 0;
    return set_breakpoints();
  }
  
  return 0;
}

static void
remote_insert_wbreakpoint (addr, signo)
     CORE_ADDR addr;
     enum target_signal signo;
{
  breakpoint[MAX_BREAKS].address = addr;
  breakpoint[MAX_BREAKS].on = 1;
  set_breakpoints();
  wbreakpoint_signo = signo;
  wbreakpoint_addr = addr;
}

static enum target_signal
remote_remove_wbreakpoint ()
{
  breakpoint[MAX_BREAKS].address = 0;
  breakpoint[MAX_BREAKS].on = 0;
  wbreakpoint_addr = (CORE_ADDR) 0;
  set_breakpoints();
  return wbreakpoint_signo;
}


/* Define the target subroutine names */

struct target_ops palmos_ops, pilot_ops;

static void
init_palmos_ops (void)
{
  palmos_ops.to_shortname = "palmos";
  palmos_ops.to_longname = "Remote target in Palm OS-specific protocol";
  palmos_ops.to_doc = "\
Use a Palm OS device via a serial line, or emulator via TCP, using\n\
a Palm OS-specific protocol.  Specify the device it is connected to\n\
(e.g., /dev/ttyS0); localhost:2000 is assumed by default.";

  palmos_ops.to_open = remote_open;
  palmos_ops.to_close = remote_close;
  palmos_ops.to_attach = NULL;
  palmos_ops.to_post_attach = NULL;  /* ? */
  palmos_ops.to_require_attach = NULL;  /* ? */
  palmos_ops.to_detach = remote_detach;
  palmos_ops.to_require_detach = NULL; /* ? */
  palmos_ops.to_resume = remote_resume;
  palmos_ops.to_wait = remote_wait;
  palmos_ops.to_post_wait = NULL; /* ? */
  palmos_ops.to_fetch_registers = remote_fetch_registers;
  palmos_ops.to_store_registers = remote_store_registers;
  palmos_ops.to_prepare_to_store = remote_prepare_to_store;
  palmos_ops.to_xfer_memory = remote_xfer_memory;
  palmos_ops.to_files_info = remote_files_info;
  palmos_ops.to_insert_breakpoint = remote_insert_breakpoint;
  palmos_ops.to_remove_breakpoint = remote_remove_breakpoint;
  palmos_ops.to_terminal_init = NULL;
  palmos_ops.to_terminal_inferior = NULL;
  palmos_ops.to_terminal_ours_for_output = NULL;
  palmos_ops.to_terminal_ours = NULL;
  palmos_ops.to_terminal_info = NULL;
  palmos_ops.to_kill = remote_kill;
  palmos_ops.to_load = generic_load;
  palmos_ops.to_lookup_symbol = NULL;
  palmos_ops.to_create_inferior = NULL;

  /* ? */
  palmos_ops.to_post_startup_inferior = NULL;
  palmos_ops.to_acknowledge_created_inferior = NULL;
  palmos_ops.to_clone_and_follow_inferior = NULL;
  palmos_ops.to_post_follow_inferior_by_clone = NULL;
  palmos_ops.to_insert_fork_catchpoint = NULL;
  palmos_ops.to_remove_fork_catchpoint = NULL;
  palmos_ops.to_insert_vfork_catchpoint = NULL;
  palmos_ops.to_remove_vfork_catchpoint = NULL;
  palmos_ops.to_has_forked = NULL;
  palmos_ops.to_has_vforked = NULL;
  palmos_ops.to_can_follow_vfork_prior_to_exec = NULL;
  palmos_ops.to_post_follow_vfork = NULL;	
  palmos_ops.to_insert_exec_catchpoint = NULL;
  palmos_ops.to_remove_exec_catchpoint = NULL;
  palmos_ops.to_has_execd = NULL;
  palmos_ops.to_reported_exec_events_per_exec_call = NULL;
  palmos_ops.to_has_exited = NULL;
  /* end ? */

  palmos_ops.to_mourn_inferior = remote_mourn;
  palmos_ops.to_can_run = 0;  /* NULL??? */
  palmos_ops.to_notice_signals = 0;
  palmos_ops.to_thread_alive = 0;
  palmos_ops.to_stop = 0;
  palmos_ops.to_pid_to_exec_file = NULL; /* ? */
  palmos_ops.to_stratum = process_stratum;
  palmos_ops.DONT_USE = NULL;  /* ?? to_next */
  palmos_ops.to_has_all_memory = 1;
  palmos_ops.to_has_memory = 1;
  palmos_ops.to_has_stack = 1;
  palmos_ops.to_has_registers = 1;
  palmos_ops.to_has_execution = 1;
  palmos_ops.to_sections = NULL;
  palmos_ops.to_sections_end = NULL;
  palmos_ops.to_magic = OPS_MAGIC;
}

void
_initialize_remote_palmos ()
{
  init_palmos_ops ();
  add_target (&palmos_ops);

  pilot_ops = palmos_ops;
  pilot_ops.to_shortname = "pilot";
  pilot_ops.to_open = remote_open_pilot;
  pilot_ops.to_mourn_inferior = remote_mourn_pilot;
  add_target (&pilot_ops);
}
