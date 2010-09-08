
/*--------------------------------------------------------------------*/
/*--- Datagrind: Track data accesses.               dg_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Datagrind, a tool for tracking data accesses.
   It borrows heavily from the Lackey tool.

   Copyright (C) 2010 Bruce Merry
      bmerry@users.sourceforge.net

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_vki.h"
#include "pub_tool_hashtable.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"

#include "datagrind.h"
#include "dg_record.h"

typedef enum
{
   EventType_Dr,
   EventType_Dw,
   EventType_Ir
} EventType;

typedef struct
{
   EventType type;
   IRExpr *addr;
   Int size;
} Event;

typedef struct
{
   VgHashNode header;
} DGDebugInfo;

#define NEVENTS 4
#define OUT_BUF_SIZE 4096

static Event events[NEVENTS];
static Int nevents = 0;
static Int out_fd = -1;
static Char out_buf[OUT_BUF_SIZE];
static UInt out_buf_used = 0;

static VgHashTable debuginfo_table = NULL;
static Bool debuginfo_dirty = True;

static Bool clo_datagrind_trace_instr = True;
static Char *clo_datagrind_out_file = "datagrind.out.%p";

static Bool dg_process_cmd_line_option(Char *arg)
{
   if (VG_STR_CLO(arg, "--datagrind-out-file", clo_datagrind_out_file)) {}
   else if (VG_BOOL_CLO(arg, "--datagrind-track-instr", clo_datagrind_trace_instr)) {}
   else
       return False;
   return True;
}

static void dg_print_usage(void)
{
   VG_(printf)(
"    --datagrind-out-file=<file>      output file name [datagrind.out]\n"
"    --datagrind-trace-instr=yes|no   trace instructions [yes]\n"
   );
}

static void dg_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

static void dg_post_clo_init(void)
{
}

static void out_flush(void)
{
   VG_(write)(out_fd, out_buf, out_buf_used);
   out_buf_used = 0;
}

static void out_bytes(const void *buf, Int count)
{
   if (count > OUT_BUF_SIZE - out_buf_used)
      out_flush();
   VG_(memcpy)(out_buf + out_buf_used, buf, count);
   out_buf_used += count;
}

static void out_byte(Char byte)
{
   if (out_buf_used >= OUT_BUF_SIZE)
      out_flush();
   out_buf[out_buf_used++] = byte;
}

static inline void out_word(UWord word)
{
   out_bytes(&word, sizeof(word));
}

static inline void trace_access(Addr addr, SizeT size, Char rtype)
{
   out_byte(rtype);
   out_byte(1 + sizeof(addr));
   out_byte(size);
   out_word(addr);
}

static VG_REGPARM(2) void trace_Dr(Addr addr, SizeT size)
{
   trace_access(addr, size, DG_R_READ);
}

static VG_REGPARM(2) void trace_Dw(Addr addr, SizeT size)
{
   trace_access(addr, size, DG_R_WRITE);
}

static VG_REGPARM(2) void trace_Ir(Addr addr, SizeT size)
{
   trace_access(addr, size, DG_R_INSTR);
}

static void flushEvents(IRSB *sb)
{
   Int i;
   static const struct
   {
      const Char *name;
      void *fn;
   } helpers[3] =
   {
      { "trace_Dr", &trace_Dr },
      { "trace_Dw", &trace_Dw },
      { "trace_Ir", &trace_Ir }
   };

   for (i = 0; i < nevents; i++)
   {
      Event *ev = &events[i];
      IRExpr **argv;
      IRDirty *di;

      argv = mkIRExprVec_2(ev->addr, mkIRExpr_HWord(ev->size));
      di = unsafeIRDirty_0_N(2, (Char *) helpers[ev->type].name,
                             VG_(fnptr_to_fnentry)(helpers[ev->type].fn),
                             argv);
      addStmtToIRSB(sb, IRStmt_Dirty(di));
   }
   nevents = 0;
}

static void addEvent(IRSB *sb, IRExpr *addr, Int size, EventType type)
{
   Event *ev;
   if (nevents == NEVENTS)
      flushEvents(sb);
   ev = &events[nevents];
   ev->type = type;
   ev->addr = addr;
   ev->size = size;
   nevents++;
}

static void addEvent_Dr(IRSB *sb, IRExpr *daddr, Int dsize)
{
   addEvent(sb, daddr, dsize, EventType_Dr);
}

static void addEvent_Dw(IRSB *sb, IRExpr *daddr, Int dsize)
{
   addEvent(sb, daddr, dsize, EventType_Dw);
}

static void addEvent_Ir(IRSB *sb, IRExpr *iaddr, Int isize)
{
   addEvent(sb, iaddr, isize, EventType_Ir);
}

static void prepare_out_file(void)
{
   if (out_fd == -1)
   {
      SysRes sres;
      Char *filename = VG_(expand_file_name)("--datagrind-out-file", clo_datagrind_out_file);
      sres = VG_(open)(filename, VKI_O_CREAT | VKI_O_TRUNC | VKI_O_WRONLY,
                       VKI_S_IRUSR | VKI_S_IWUSR);
      if (sr_isError(sres))
      {
         VG_(message)(Vg_UserMsg,
                      "Error: can not open datagrind output file `%s'\n",
                      filename);
         VG_(exit)(1);
      }
      else
      {
         static const Char magic[] = "DATAGRIND1";

         out_fd = (Int) sr_Res(sres);
         out_byte(DG_R_HEADER);
         out_byte(sizeof(magic) + 3);
         out_bytes(magic, sizeof(magic));
         out_byte(1); /* version */
#if VG_BIGENDIAN
         out_byte(1);
#elif VG_LITTLEENDIAN
         out_byte(0);
#else
         tl_assert(0);
#endif
         out_byte(VG_WORDSIZE);
      }
   }
}

static void clean_debuginfo(void)
{
   if (debuginfo_dirty)
   {
      const DebugInfo *di = VG_(next_DebugInfo)(NULL);

      if (debuginfo_table == NULL)
      {
         debuginfo_table = VG_(HT_construct)("debuginfo_table");
      }

      for (; di != NULL; di = VG_(next_DebugInfo)(di))
      {
         const UChar *filename = VG_(DebugInfo_get_filename)(di);
         Addr text_avma = VG_(DebugInfo_get_text_avma)(di);
         PtrdiffT text_bias = VG_(DebugInfo_get_text_bias)(di);
         SizeT filename_len = VG_(strlen)(filename);

         if (!VG_(HT_lookup)(debuginfo_table, (UWord) di))
         {
            DGDebugInfo *node = VG_(calloc)("debuginfo_table.node", 1, sizeof(DGDebugInfo));
            node->header.key = (UWord) di;
            VG_(umsg)("Filename = %s\ntext_avma = %#lx\ntext_bias = %ld\n",
                      filename, text_avma, text_bias);
            VG_(HT_add_node)(debuginfo_table, node);

            if (filename_len > 128) filename_len = 128;
            out_byte(DG_R_TEXT_AVMA);
            out_byte(filename_len + sizeof(Addr) + 1);
            out_word(text_avma);
            out_bytes(filename, filename_len);
            out_byte('\0');
         }
      }
      debuginfo_dirty = False;
   }
}

static IRSB* dg_instrument(VgCallbackClosure* closure,
                           IRSB* sbIn,
                           VexGuestLayout* layout,
                           VexGuestExtents* vge,
                           IRType gWordTy, IRType hWordTy)
{
   IRSB* sbOut;
   Int i;

   if (gWordTy != hWordTy)
   {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   prepare_out_file();
   clean_debuginfo();

   sbOut = deepCopyIRSBExceptStmts(sbIn);

   /* Copy preamble */
   for (i = 0; i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark; i++)
   {
      addStmtToIRSB(sbOut, sbIn->stmts[i]);
   }

   for (; i < sbIn->stmts_used; i++)
   {
      IRStmt *st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag)
      {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            break;
         case Ist_Exit:
            flushEvents(sbOut);
            break;
         case Ist_IMark:
            if (clo_datagrind_trace_instr)
            {
                addEvent_Ir(sbOut, mkIRExpr_HWord((HWord) st->Ist.IMark.addr),
                            st->Ist.IMark.len);
            }
            break;
         case Ist_WrTmp:
            {
               IRExpr* data = st->Ist.WrTmp.data;
               if (data->tag == Iex_Load)
               {
                  addEvent_Dr(sbOut, data->Iex.Load.addr, sizeofIRType(data->Iex.Load.ty));
               }
            }
            break;
         case Ist_Store:
            {
               IRExpr* data = st->Ist.Store.data;
               addEvent_Dw(sbOut, st->Ist.Store.addr,
                           sizeofIRType(typeOfIRExpr(sbOut->tyenv, data)));
            }
            break;
         case Ist_Dirty:
            {
               IRDirty* d = st->Ist.Dirty.details;
               if (d->mFx != Ifx_None)
               {
                  tl_assert(d->mAddr != NULL);
                  tl_assert(d->mSize != 0);
                  if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                     addEvent_Dr(sbOut, d->mAddr, d->mSize);
                  if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                     addEvent_Dw(sbOut, d->mAddr, d->mSize);
               }
            }
            break;
         case Ist_CAS:
            {
               Int dataSize;
               IRCAS *cas = st->Ist.CAS.details;
               tl_assert(cas->addr != NULL);
               tl_assert(cas->dataLo != NULL);
               dataSize = sizeofIRType(typeOfIRExpr(sbOut->tyenv, cas->dataLo));
               if (cas->dataHi != NULL)
                  dataSize *= 2;
               addEvent_Dr(sbOut, cas->addr, dataSize);
               addEvent_Dw(sbOut, cas->addr, dataSize);
            }
            break;
         default:
            tl_assert(0);
      }
      addStmtToIRSB(sbOut, st);
   }

   flushEvents(sbOut);

   return sbOut;
}

static Bool dg_handle_client_request(ThreadId tid, UWord *args, UWord *ret)
{
   if (!VG_IS_TOOL_USERREQ('D', 'G', args[0]))
      return False;

   switch (args[0])
   {
   case VG_USERREQ__TRACK_RANGE:
      {
         UWord addr = args[1];
         UWord len = args[2];
         const Char *type = (const Char *) args[3];
         const Char *label = (const Char *) args[4];
         SizeT type_len = VG_(strlen)(type);
         SizeT label_len = VG_(strlen)(label);

         if (type_len > 64) type_len = 64;
         if (label_len > 64) label_len = 64;
         out_byte(DG_R_TRACK_RANGE);
         out_byte(2 * sizeof(addr) + type_len + label_len + 2);
         out_word(addr);
         out_word(len);
         out_bytes(type, type_len);
         out_byte('\0');
         out_bytes(label, label_len);
         out_byte('\0');
      }
      break;
   case VG_USERREQ__UNTRACK_RANGE:
      {
          UWord addr = args[1];
          UWord len = args[2];
          out_byte(DG_R_UNTRACK_RANGE);
          out_byte(2 * sizeof(addr));
          out_word(addr);
          out_word(len);
      }
      break;
   case VG_USERREQ__START_EVENT:
   case VG_USERREQ__END_EVENT:
      {
          const Char *label = (const Char *) args[1];
          SizeT label_len = VG_(strlen)(label);
          if (label_len > 64) label_len = 64;
          out_byte(args[0] == VG_USERREQ__START_EVENT ? DG_R_START_EVENT : DG_R_END_EVENT);
          out_byte(label_len + 1);
          out_bytes(label, label_len);
          out_byte('\0');
      }
      break;
   default:
      return False;
   }
   return True;
}

static void dg_track_new_mem_mmap_or_startup(Addr a, SizeT len, Bool rr, Bool ww, Bool xx, ULong di_handle)
{
   if (xx)
      debuginfo_dirty = True;
}

static void dg_fini(Int exitcode)
{
   if (out_fd != -1)
   {
      out_flush();
      VG_(close)(out_fd);
   }

   /* TODO: need to free the node entries */
   if (debuginfo_table != NULL)
      VG_(HT_destruct)(debuginfo_table);
}

static void dg_pre_clo_init(void)
{
   VG_(details_name)            ("Datagrind");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("tracks data accesses");
   VG_(details_copyright_author)(
      "Copyright (C) 2010, and GNU GPL'd, by Bruce Merry.");
   VG_(details_bug_reports_to)  ("bmerry@users.sourceforge.net");

   VG_(basic_tool_funcs)        (dg_post_clo_init,
                                 dg_instrument,
                                 dg_fini);

   VG_(needs_command_line_options)(dg_process_cmd_line_option,
                                   dg_print_usage,
                                   dg_print_debug_usage);
   VG_(needs_client_requests)(dg_handle_client_request);
   VG_(track_new_mem_startup)(dg_track_new_mem_mmap_or_startup);
   VG_(track_new_mem_mmap)(dg_track_new_mem_mmap_or_startup);
}

VG_DETERMINE_INTERFACE_VERSION(dg_pre_clo_init)
