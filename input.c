/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <ctype.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"

/*
 * Based on the description by Paul Williams at:
 *
 * https://vt100.net/emu/dec_ansi_parser
 *
 * With the following changes:
 *
 * - 7-bit only.
 *
 * - Support for UTF-8.
 *
 * - OSC (but not APC) may be terminated by \007 as well as ST.
 *
 * - A state for APC similar to OSC. Some terminals appear to use this to set
 *   the title.
 *
 * - A state for the screen \033k...\033\\ sequence to rename a window. This is
 *   pretty stupid but not supporting it is more trouble than it is worth.
 *
 * - Special handling for ESC inside a DCS to allow arbitrary byte sequences to
 *   be passed to the underlying terminals.
 */

/* Input parser cell. */
struct input_cell {
	struct grid_cell	cell;
	int			set;
	int			g0set;	/* 1 if ACS */
	int			g1set;	/* 1 if ACS */
};

/* Input parser argument. */
struct input_param {
	enum {
		INPUT_MISSING,
		INPUT_NUMBER,
		INPUT_STRING
	}			type;
	union {
		int		num;
		char	       *str;
	};
};

/* Input parser context. */
struct input_ctx {
	struct window_pane     *wp;
	struct bufferevent     *event;
	struct screen_write_ctx ctx;
	struct colour_palette  *palette;

	int			term_level;
	int			max_level;

	struct input_cell	cell;

	struct input_cell	old_cell;
	u_int			old_cx;
	u_int			old_cy;
	int			old_mode;

	u_char			interm_buf[4];
	size_t			interm_len;

	u_char			param_buf[64];
	size_t			param_len;

#define INPUT_BUF_START 32
	u_char		       *input_buf;
	size_t			input_len;
	size_t			input_space;
	enum {
		INPUT_END_ST,
		INPUT_END_BEL
	}			input_end;

	struct input_param	param_list[24];
	u_int			param_list_len;

	struct utf8_data	utf8data;
	int			utf8started;

	int			ch;
	struct utf8_data	last;

	int			flags;
#define INPUT_DISCARD 0x1
#define INPUT_LAST 0x2

	const struct input_state *state;

	struct event		timer;

	/*
	 * All input received since we were last in the ground state. Sent to
	 * control clients on connection.
	 */
	struct evbuffer		*since_ground;
};

/* Helper functions. */
struct input_transition;
static int	input_split(struct input_ctx *);
static int	input_get(struct input_ctx *, u_int, int, int);
static void printflike(2, 3) input_reply(struct input_ctx *, const char *, ...);
static void	input_reply_decrpss_sgr(struct input_ctx *);
static void	input_reply_deccir(struct input_ctx *);
static void	input_reply_dectabsr(struct input_ctx *);
static void	input_reply_decctr(struct input_ctx *);
static void	input_set_state(struct input_ctx *,
		    const struct input_transition *);
static void	input_reset_cell(struct input_ctx *);
static void	input_report_current_theme(struct input_ctx *);
static void	input_soft_reset(struct input_ctx *);

static void	input_osc_4(struct input_ctx *, const char *);
static void	input_osc_8(struct input_ctx *, const char *);
static void	input_osc_10(struct input_ctx *, const char *);
static void	input_osc_11(struct input_ctx *, const char *);
static void	input_osc_12(struct input_ctx *, const char *);
static void	input_osc_52(struct input_ctx *, const char *);
static void	input_osc_104(struct input_ctx *, const char *);
static void	input_osc_110(struct input_ctx *, const char *);
static void	input_osc_111(struct input_ctx *, const char *);
static void	input_osc_112(struct input_ctx *, const char *);
static void	input_osc_133(struct input_ctx *, const char *);

/* Transition entry/exit handlers. */
static void	input_clear(struct input_ctx *);
static void	input_ground(struct input_ctx *);
static void	input_enter_dcs(struct input_ctx *);
static void	input_enter_osc(struct input_ctx *);
static void	input_exit_osc(struct input_ctx *);
static void	input_enter_apc(struct input_ctx *);
static void	input_exit_apc(struct input_ctx *);
static void	input_enter_rename(struct input_ctx *);
static void	input_exit_rename(struct input_ctx *);

/* Input state handlers. */
static int	input_print(struct input_ctx *);
static int	input_intermediate(struct input_ctx *);
static int	input_parameter(struct input_ctx *);
static int	input_input(struct input_ctx *);
static int	input_c0_dispatch(struct input_ctx *);
static int	input_esc_dispatch(struct input_ctx *);
static int	input_csi_dispatch(struct input_ctx *);
static void	input_csi_dispatch_rm(struct input_ctx *);
static void	input_csi_dispatch_rm_private(struct input_ctx *);
static void	input_csi_dispatch_sm(struct input_ctx *);
static void	input_csi_dispatch_sm_private(struct input_ctx *);
static void	input_csi_dispatch_sm_graphics(struct input_ctx *);
static void	input_csi_dispatch_decrqm(struct input_ctx *);
static void	input_csi_dispatch_decrqm_private(struct input_ctx *);
static void	input_csi_dispatch_winops(struct input_ctx *);
static void	input_csi_dispatch_sgr_256(struct input_ctx *, int, u_int *);
static void	input_csi_dispatch_sgr_rgb(struct input_ctx *, int, u_int *);
static void	input_csi_dispatch_sgr(struct input_ctx *);
static void	input_csi_dispatch_decrqpsr(struct input_ctx *);
static void	input_csi_dispatch_decrqtsr(struct input_ctx *);
static int	input_dcs_dispatch(struct input_ctx *);
static void	input_dcs_dispatch_decrqss(struct input_ctx *);
static void	input_dcs_dispatch_decrsps(struct input_ctx *);
static void	input_dcs_dispatch_deccir(struct input_ctx *);
static void	input_dcs_dispatch_dectabsr(struct input_ctx *);
static void	input_dcs_dispatch_decrsts(struct input_ctx *);
static void	input_dcs_dispatch_decctr(struct input_ctx *);
static int	input_top_bit_set(struct input_ctx *);
static int	input_end_bel(struct input_ctx *);

/* Command table comparison function. */
static int	input_table_compare(const void *, const void *);

/* Command table entry. */
struct input_table_entry {
	int		ch;
	const char     *interm;
	int		type;
};

/* Escape commands. */
enum input_esc_type {
	INPUT_ESC_DECALN,
	INPUT_ESC_DECBI,
	INPUT_ESC_DECFI,
	INPUT_ESC_DECKPAM,
	INPUT_ESC_DECKPNM,
	INPUT_ESC_DECRC,
	INPUT_ESC_DECSC,
	INPUT_ESC_HTS,
	INPUT_ESC_IND,
	INPUT_ESC_NEL,
	INPUT_ESC_RI,
	INPUT_ESC_RIS,
	INPUT_ESC_SCSG0_OFF,
	INPUT_ESC_SCSG0_ON,
	INPUT_ESC_SCSG1_OFF,
	INPUT_ESC_SCSG1_ON,
	INPUT_ESC_ST
};

/* Escape command table. */
static const struct input_table_entry input_esc_table[] = {
	{ '0', "(", INPUT_ESC_SCSG0_ON },
	{ '0', ")", INPUT_ESC_SCSG1_ON },
	{ '6', "",  INPUT_ESC_DECBI },
	{ '7', "",  INPUT_ESC_DECSC },
	{ '8', "",  INPUT_ESC_DECRC },
	{ '8', "#", INPUT_ESC_DECALN },
	{ '9', "",  INPUT_ESC_DECFI },
	{ '=', "",  INPUT_ESC_DECKPAM },
	{ '>', "",  INPUT_ESC_DECKPNM },
	{ 'B', "(", INPUT_ESC_SCSG0_OFF },
	{ 'B', ")", INPUT_ESC_SCSG1_OFF },
	{ 'D', "",  INPUT_ESC_IND },
	{ 'E', "",  INPUT_ESC_NEL },
	{ 'H', "",  INPUT_ESC_HTS },
	{ 'M', "",  INPUT_ESC_RI },
	{ '\\', "", INPUT_ESC_ST },
	{ 'c', "",  INPUT_ESC_RIS },
};

/* Control (CSI) commands. */
enum input_csi_type {
	INPUT_CSI_CBT,
	INPUT_CSI_CHT,
	INPUT_CSI_CNL,
	INPUT_CSI_CPL,
	INPUT_CSI_CUB,
	INPUT_CSI_CUD,
	INPUT_CSI_CUF,
	INPUT_CSI_CUP,
	INPUT_CSI_CUU,
	INPUT_CSI_DA,
	INPUT_CSI_DA_TWO,
	INPUT_CSI_DCH,
	INPUT_CSI_DECDC,
	INPUT_CSI_DECIC,
	INPUT_CSI_DECRQM,
	INPUT_CSI_DECRQM_PRIVATE,
	INPUT_CSI_DECRQPSR,
	INPUT_CSI_DECRQTSR,
	INPUT_CSI_DECSCA,
	INPUT_CSI_DECSCL,
	INPUT_CSI_DECSCUSR,
	INPUT_CSI_DECSED,
	INPUT_CSI_DECSEL,
	INPUT_CSI_DECSTBM,
	INPUT_CSI_DECSTR,
	INPUT_CSI_DL,
	INPUT_CSI_DSR,
	INPUT_CSI_DSR_PRIVATE,
	INPUT_CSI_ECH,
	INPUT_CSI_ED,
	INPUT_CSI_EL,
	INPUT_CSI_HPA,
	INPUT_CSI_ICH,
	INPUT_CSI_IL,
	INPUT_CSI_MODOFF,
	INPUT_CSI_MODSET,
	INPUT_CSI_RCP,
	INPUT_CSI_REP,
	INPUT_CSI_RM,
	INPUT_CSI_RM_PRIVATE,
	INPUT_CSI_SCP_DECSLRM,
	INPUT_CSI_SD,
	INPUT_CSI_SGR,
	INPUT_CSI_SL,
	INPUT_CSI_SM,
	INPUT_CSI_SM_GRAPHICS,
	INPUT_CSI_SM_PRIVATE,
	INPUT_CSI_SR,
	INPUT_CSI_SU,
	INPUT_CSI_TBC,
	INPUT_CSI_VPA,
	INPUT_CSI_WINOPS,
	INPUT_CSI_XDA
};

/* Control (CSI) command table. */
static const struct input_table_entry input_csi_table[] = {
	{ '@', "",   INPUT_CSI_ICH },
	{ '@', " ",  INPUT_CSI_SL },
	{ 'A', "",   INPUT_CSI_CUU },
	{ 'A', " ",  INPUT_CSI_SR },
	{ 'B', "",   INPUT_CSI_CUD },
	{ 'C', "",   INPUT_CSI_CUF },
	{ 'D', "",   INPUT_CSI_CUB },
	{ 'E', "",   INPUT_CSI_CNL },
	{ 'F', "",   INPUT_CSI_CPL },
	{ 'G', "",   INPUT_CSI_HPA },
	{ 'H', "",   INPUT_CSI_CUP },
	{ 'I', "",   INPUT_CSI_CHT },
	{ 'J', "",   INPUT_CSI_ED },
	{ 'J', "?",  INPUT_CSI_DECSED },
	{ 'K', "",   INPUT_CSI_EL },
	{ 'K', "?",  INPUT_CSI_DECSEL },
	{ 'L', "",   INPUT_CSI_IL },
	{ 'M', "",   INPUT_CSI_DL },
	{ 'P', "",   INPUT_CSI_DCH },
	{ 'S', "",   INPUT_CSI_SU },
	{ 'S', "?",  INPUT_CSI_SM_GRAPHICS },
	{ 'T', "",   INPUT_CSI_SD },
	{ 'X', "",   INPUT_CSI_ECH },
	{ 'Z', "",   INPUT_CSI_CBT },
	{ '`', "",   INPUT_CSI_HPA },
	{ 'a', "",   INPUT_CSI_CUF },
	{ 'b', "",   INPUT_CSI_REP },
	{ 'c', "",   INPUT_CSI_DA },
	{ 'c', ">",  INPUT_CSI_DA_TWO },
	{ 'd', "",   INPUT_CSI_VPA },
	{ 'e', "",   INPUT_CSI_CUD },
	{ 'f', "",   INPUT_CSI_CUP },
	{ 'g', "",   INPUT_CSI_TBC },
	{ 'h', "",   INPUT_CSI_SM },
	{ 'h', "?",  INPUT_CSI_SM_PRIVATE },
	{ 'j', "",   INPUT_CSI_CUB },
	{ 'k', "",   INPUT_CSI_CUU },
	{ 'l', "",   INPUT_CSI_RM },
	{ 'l', "?",  INPUT_CSI_RM_PRIVATE },
	{ 'm', "",   INPUT_CSI_SGR },
	{ 'm', ">",  INPUT_CSI_MODSET },
	{ 'n', "",   INPUT_CSI_DSR },
	{ 'n', ">",  INPUT_CSI_MODOFF },
	{ 'n', "?",  INPUT_CSI_DSR_PRIVATE },
	{ 'p', "!",  INPUT_CSI_DECSTR },
	{ 'p', "\"", INPUT_CSI_DECSCL },
	{ 'p', "$",  INPUT_CSI_DECRQM },
	{ 'p', "?$", INPUT_CSI_DECRQM_PRIVATE },
	{ 'q', " ",  INPUT_CSI_DECSCUSR },
	{ 'q', "\"", INPUT_CSI_DECSCA },
	{ 'q', ">",  INPUT_CSI_XDA },
	{ 'r', "",   INPUT_CSI_DECSTBM },
	{ 's', "",   INPUT_CSI_SCP_DECSLRM },
	{ 't', "",   INPUT_CSI_WINOPS },
	{ 'u', "",   INPUT_CSI_RCP },
	{ 'u', "$",  INPUT_CSI_DECRQTSR },
	{ 'w', "$",  INPUT_CSI_DECRQPSR },
	{ '}', "'",  INPUT_CSI_DECIC },
	{ '~', "'",  INPUT_CSI_DECDC }
};

/* Device Control (DCS) commands. */
enum input_dcs_type {
	INPUT_DCS_DECRQSS,
	INPUT_DCS_DECRSPS,
	INPUT_DCS_DECRSTS,
	INPUT_DCS_SIXEL
};

/* Device Control (DCS) command table. */
static const struct input_table_entry input_dcs_table[] = {
	{ 'p', "$", INPUT_DCS_DECRSTS },
#ifdef ENABLE_SIXEL
	{ 'q', "",  INPUT_DCS_SIXEL },
#endif
	{ 'q', "$", INPUT_DCS_DECRQSS },
	{ 't', "$", INPUT_DCS_DECRSPS }
};

/* Input transition. */
struct input_transition {
	int				first;
	int				last;

	int				(*handler)(struct input_ctx *);
	const struct input_state       *state;
};

/* Input state. */
struct input_state {
	const char			*name;
	void				(*enter)(struct input_ctx *);
	void				(*exit)(struct input_ctx *);
	const struct input_transition	*transitions;
};

/* State transitions available from all states. */
#define INPUT_STATE_ANYWHERE \
	{ 0x18, 0x18, input_c0_dispatch, &input_state_ground }, \
	{ 0x1a, 0x1a, input_c0_dispatch, &input_state_ground }, \
	{ 0x1b, 0x1b, NULL,		 &input_state_esc_enter }

/* Forward declarations of state tables. */
static const struct input_transition input_state_ground_table[];
static const struct input_transition input_state_esc_enter_table[];
static const struct input_transition input_state_esc_intermediate_table[];
static const struct input_transition input_state_csi_enter_table[];
static const struct input_transition input_state_csi_parameter_table[];
static const struct input_transition input_state_csi_intermediate_table[];
static const struct input_transition input_state_csi_ignore_table[];
static const struct input_transition input_state_dcs_enter_table[];
static const struct input_transition input_state_dcs_parameter_table[];
static const struct input_transition input_state_dcs_intermediate_table[];
static const struct input_transition input_state_dcs_handler_table[];
static const struct input_transition input_state_dcs_escape_table[];
static const struct input_transition input_state_dcs_ignore_table[];
static const struct input_transition input_state_decrqss_enter_table[];
static const struct input_transition input_state_decrqss_intermediate_table[];
static const struct input_transition input_state_decrqss_ignore_table[];
static const struct input_transition input_state_osc_string_table[];
static const struct input_transition input_state_apc_string_table[];
static const struct input_transition input_state_rename_string_table[];
static const struct input_transition input_state_consume_st_table[];

/* ground state definition. */
static const struct input_state input_state_ground = {
	"ground",
	input_ground, NULL,
	input_state_ground_table
};

/* esc_enter state definition. */
static const struct input_state input_state_esc_enter = {
	"esc_enter",
	input_clear, NULL,
	input_state_esc_enter_table
};

/* esc_intermediate state definition. */
static const struct input_state input_state_esc_intermediate = {
	"esc_intermediate",
	NULL, NULL,
	input_state_esc_intermediate_table
};

/* csi_enter state definition. */
static const struct input_state input_state_csi_enter = {
	"csi_enter",
	input_clear, NULL,
	input_state_csi_enter_table
};

/* csi_parameter state definition. */
static const struct input_state input_state_csi_parameter = {
	"csi_parameter",
	NULL, NULL,
	input_state_csi_parameter_table
};

/* csi_intermediate state definition. */
static const struct input_state input_state_csi_intermediate = {
	"csi_intermediate",
	NULL, NULL,
	input_state_csi_intermediate_table
};

/* csi_ignore state definition. */
static const struct input_state input_state_csi_ignore = {
	"csi_ignore",
	NULL, NULL,
	input_state_csi_ignore_table
};

/* dcs_enter state definition. */
static const struct input_state input_state_dcs_enter = {
	"dcs_enter",
	input_enter_dcs, NULL,
	input_state_dcs_enter_table
};

/* dcs_parameter state definition. */
static const struct input_state input_state_dcs_parameter = {
	"dcs_parameter",
	NULL, NULL,
	input_state_dcs_parameter_table
};

/* dcs_intermediate state definition. */
static const struct input_state input_state_dcs_intermediate = {
	"dcs_intermediate",
	NULL, NULL,
	input_state_dcs_intermediate_table
};

/* dcs_handler state definition. */
static const struct input_state input_state_dcs_handler = {
	"dcs_handler",
	NULL, NULL,
	input_state_dcs_handler_table
};

/* dcs_escape state definition. */
static const struct input_state input_state_dcs_escape = {
	"dcs_escape",
	NULL, NULL,
	input_state_dcs_escape_table
};

/* dcs_ignore state definition. */
static const struct input_state input_state_dcs_ignore = {
	"dcs_ignore",
	NULL, NULL,
	input_state_dcs_ignore_table
};

/* decrqss_enter state definition. */
static const struct input_state input_state_decrqss_enter = {
	"decrqss_enter",
	input_clear, NULL,
	input_state_decrqss_enter_table
};

/* decrqss_intermediate state definition. */
static const struct input_state input_state_decrqss_intermediate = {
	"decrqss_intermediate",
	NULL, NULL,
	input_state_decrqss_intermediate_table
};

/* decrqss_ignore state definition. */
static const struct input_state input_state_decrqss_ignore = {
	"decrqss_ignore",
	NULL, NULL,
	input_state_decrqss_ignore_table
};

/* osc_string state definition. */
static const struct input_state input_state_osc_string = {
	"osc_string",
	input_enter_osc, input_exit_osc,
	input_state_osc_string_table
};

/* apc_string state definition. */
static const struct input_state input_state_apc_string = {
	"apc_string",
	input_enter_apc, input_exit_apc,
	input_state_apc_string_table
};

/* rename_string state definition. */
static const struct input_state input_state_rename_string = {
	"rename_string",
	input_enter_rename, input_exit_rename,
	input_state_rename_string_table
};

/* consume_st state definition. */
static const struct input_state input_state_consume_st = {
	"consume_st",
	input_enter_rename, NULL, /* rename also waits for ST */
	input_state_consume_st_table
};

/* ground state table. */
static const struct input_transition input_state_ground_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch, NULL },
	{ 0x19, 0x19, input_c0_dispatch, NULL },
	{ 0x1c, 0x1f, input_c0_dispatch, NULL },
	{ 0x20, 0x7e, input_print,	 NULL },
	{ 0x7f, 0x7f, NULL,		 NULL },
	{ 0x80, 0xff, input_top_bit_set, NULL },

	{ -1, -1, NULL, NULL }
};

/* esc_enter state table. */
static const struct input_transition input_state_esc_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_esc_intermediate },
	{ 0x30, 0x4f, input_esc_dispatch, &input_state_ground },
	{ 0x50, 0x50, NULL,		  &input_state_dcs_enter },
	{ 0x51, 0x57, input_esc_dispatch, &input_state_ground },
	{ 0x58, 0x58, NULL,		  &input_state_consume_st },
	{ 0x59, 0x59, input_esc_dispatch, &input_state_ground },
	{ 0x5a, 0x5a, input_esc_dispatch, &input_state_ground },
	{ 0x5b, 0x5b, NULL,		  &input_state_csi_enter },
	{ 0x5c, 0x5c, input_esc_dispatch, &input_state_ground },
	{ 0x5d, 0x5d, NULL,		  &input_state_osc_string },
	{ 0x5e, 0x5e, NULL,		  &input_state_consume_st },
	{ 0x5f, 0x5f, NULL,		  &input_state_apc_string },
	{ 0x60, 0x6a, input_esc_dispatch, &input_state_ground },
	{ 0x6b, 0x6b, NULL,		  &input_state_rename_string },
	{ 0x6c, 0x7e, input_esc_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* esc_intermediate state table. */
static const struct input_transition input_state_esc_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x7e, input_esc_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_enter state table. */
static const struct input_transition input_state_csi_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_csi_intermediate },
	{ 0x30, 0x39, input_parameter,	  &input_state_csi_parameter },
	{ 0x3a, 0x3a, input_parameter,	  &input_state_csi_parameter },
	{ 0x3b, 0x3b, input_parameter,	  &input_state_csi_parameter },
	{ 0x3c, 0x3f, input_intermediate, &input_state_csi_parameter },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_parameter state table. */
static const struct input_transition input_state_csi_parameter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_csi_intermediate },
	{ 0x30, 0x39, input_parameter,	  NULL },
	{ 0x3a, 0x3a, input_parameter,	  NULL },
	{ 0x3b, 0x3b, input_parameter,	  NULL },
	{ 0x3c, 0x3f, NULL,		  &input_state_csi_ignore },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_intermediate state table. */
static const struct input_transition input_state_csi_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x3f, NULL,		  &input_state_csi_ignore },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_ignore state table. */
static const struct input_transition input_state_csi_ignore_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch, NULL },
	{ 0x19, 0x19, input_c0_dispatch, NULL },
	{ 0x1c, 0x1f, input_c0_dispatch, NULL },
	{ 0x20, 0x3f, NULL,		 NULL },
	{ 0x40, 0x7e, NULL,		 &input_state_ground },
	{ 0x7f, 0xff, NULL,		 NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_enter state table. */
static const struct input_transition input_state_dcs_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_dcs_intermediate },
	{ 0x30, 0x39, input_parameter,	  &input_state_dcs_parameter },
	{ 0x3a, 0x3a, NULL,		  &input_state_dcs_ignore },
	{ 0x3b, 0x3b, input_parameter,	  &input_state_dcs_parameter },
	{ 0x3c, 0x3f, input_intermediate, &input_state_dcs_parameter },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_parameter state table. */
static const struct input_transition input_state_dcs_parameter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_dcs_intermediate },
	{ 0x30, 0x39, input_parameter,	  NULL },
	{ 0x3a, 0x3a, NULL,		  &input_state_dcs_ignore },
	{ 0x3b, 0x3b, input_parameter,	  NULL },
	{ 0x3c, 0x3f, NULL,		  &input_state_dcs_ignore },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_intermediate state table. */
static const struct input_transition input_state_dcs_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x3f, NULL,		  &input_state_dcs_ignore },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_handler state table. */
static const struct input_transition input_state_dcs_handler_table[] = {
	/* No INPUT_STATE_ANYWHERE */

	{ 0x00, 0x1a, input_input,  NULL },
	{ 0x1b, 0x1b, NULL,	    &input_state_dcs_escape },
	{ 0x1c, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_escape state table. */
static const struct input_transition input_state_dcs_escape_table[] = {
	/* No INPUT_STATE_ANYWHERE */

	{ 0x00, 0x5b, input_input,	  &input_state_dcs_handler },
	{ 0x5c, 0x5c, input_dcs_dispatch, &input_state_ground },
	{ 0x5d, 0xff, input_input,	  &input_state_dcs_handler },

	{ -1, -1, NULL, NULL }
};

/* dcs_ignore state table. */
static const struct input_transition input_state_dcs_ignore_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, NULL,	    NULL },

	{ -1, -1, NULL, NULL }
};

/* decrqss_enter state table. */
static const struct input_transition input_state_decrqss_enter_table[] = {
	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x18, 0x18, NULL,		  &input_state_decrqss_ignore },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1a, 0x1b, NULL,		  &input_state_decrqss_ignore },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_decrqss_intermediate },
	{ 0x30, 0x3b, NULL,		  &input_state_decrqss_ignore },
	{ 0x3c, 0x3f, input_intermediate, &input_state_decrqss_intermediate },
	{ 0x40, 0x7e, input_input,	  &input_state_decrqss_ignore },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* decrqss_intermediate state table. */
static const struct input_transition
input_state_decrqss_intermediate_table[] = {
	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x18, 0x18, NULL,		  &input_state_decrqss_ignore },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1a, 0x1b, NULL,		  &input_state_decrqss_ignore },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x3f, NULL,		  &input_state_decrqss_ignore },
	{ 0x40, 0x7e, input_input,	  &input_state_decrqss_ignore },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* decrqss_ignore state table. */
static const struct input_transition input_state_decrqss_ignore_table[] = {
	{ 0x00, 0x7e, NULL, NULL },
	{ 0x7f, 0xff, NULL, NULL },

	{ -1, -1, NULL, NULL }
};

/* osc_string state table. */
static const struct input_transition input_state_osc_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x06, NULL,	     NULL },
	{ 0x07, 0x07, input_end_bel, &input_state_ground },
	{ 0x08, 0x17, NULL,	     NULL },
	{ 0x19, 0x19, NULL,	     NULL },
	{ 0x1c, 0x1f, NULL,	     NULL },
	{ 0x20, 0xff, input_input,   NULL },

	{ -1, -1, NULL, NULL }
};

/* apc_string state table. */
static const struct input_transition input_state_apc_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* rename_string state table. */
static const struct input_transition input_state_rename_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* consume_st state table. */
static const struct input_transition input_state_consume_st_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, NULL,	    NULL },

	{ -1, -1, NULL, NULL }
};

/* Maximum of bytes allowed to read in a single input. */
static size_t input_buffer_size = INPUT_BUF_DEFAULT_SIZE;

/* Input table compare. */
static int
input_table_compare(const void *key, const void *value)
{
	const struct input_ctx		*ictx = key;
	const struct input_table_entry	*entry = value;

	if (ictx->ch != entry->ch)
		return (ictx->ch - entry->ch);
	return (strcmp(ictx->interm_buf, entry->interm));
}

/* Stop UTF-8 and enter an invalid character. */
static void
input_stop_utf8(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	static struct utf8_data	 rc = { "\357\277\275", 3, 3, 1 };

	if (ictx->utf8started) {
		utf8_copy(&ictx->cell.cell.data, &rc);
		screen_write_collect_add(sctx, &ictx->cell.cell);
	}
	ictx->utf8started = 0;
}

/*
 * Timer - if this expires then have been waiting for a terminator for too
 * long, so reset to ground.
 */
static void
input_timer_callback(__unused int fd, __unused short events, void *arg)
{
	struct input_ctx	*ictx = arg;

	log_debug("%s: %s expired" , __func__, ictx->state->name);
	input_reset(ictx, 0);
}

/* Start the timer. */
static void
input_start_timer(struct input_ctx *ictx)
{
	struct timeval	tv = { .tv_sec = 5, .tv_usec = 0 };

	event_del(&ictx->timer);
	event_add(&ictx->timer, &tv);
}

/* Reset cell state to default. */
static void
input_reset_cell(struct input_ctx *ictx)
{
	memcpy(&ictx->cell.cell, &grid_default_cell, sizeof ictx->cell.cell);
	ictx->cell.set = 0;
	ictx->cell.g0set = ictx->cell.g1set = 0;

	memcpy(&ictx->old_cell, &ictx->cell, sizeof ictx->old_cell);
	ictx->old_cx = 0;
	ictx->old_cy = 0;
	ictx->old_mode = 0;
}

/* Perform a soft reset of the PTY. */
static void
input_soft_reset(struct input_ctx *ictx)
{
	input_reset_cell(ictx);
	screen_write_softreset(&ictx->ctx);
}

/* Save screen state. */
static void
input_save_state(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct screen		*s = sctx->s;

	memcpy(&ictx->old_cell, &ictx->cell, sizeof ictx->old_cell);
	ictx->old_cx = s->cx;
	ictx->old_cy = s->cy;
	ictx->old_mode = s->mode;
}

/* Restore screen state. */
static void
input_restore_state(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;

	memcpy(&ictx->cell, &ictx->old_cell, sizeof ictx->cell);
	if (ictx->old_mode & MODE_ORIGIN)
		screen_write_mode_set(sctx, MODE_ORIGIN);
	else
		screen_write_mode_clear(sctx, MODE_ORIGIN);
	screen_write_cursormove(sctx, ictx->old_cx, ictx->old_cy, 0);
}

#ifdef ENABLE_SIXEL
/* Return whether or not the given terminal type is a graphics-capable one. */
static int
input_is_graphics_term(int term)
{
	return (term == TERM_VT125 || term == TERM_VT241);
}
#endif

/* Initialise input parser. */
struct input_ctx *
input_init(struct window_pane *wp, struct bufferevent *bev,
    struct colour_palette *palette)
{
	struct input_ctx	*ictx;

	ictx = xcalloc(1, sizeof *ictx);
	ictx->wp = wp;
	ictx->event = bev;
	ictx->palette = palette;

	if (wp) {
		ictx->max_level = options_get_number(wp->options,
		    "default-emulation-level");
		switch (ictx->max_level) {
		case TERM_VT132:
			log_debug("%s: unsupported emulation VT131/132",
			    __func__);
#ifdef ENABLE_SIXEL
			ictx->max_level = TERM_VT241;
#else
			ictx->max_level = TERM_VT220;
#endif
			break;
		}
	} else {
#ifdef ENABLE_SIXEL
		ictx->max_level = TERM_VT241;
#else
		ictx->max_level = TERM_VT220;
#endif
	}
	ictx->term_level = ictx->max_level;

	ictx->input_space = INPUT_BUF_START;
	ictx->input_buf = xmalloc(INPUT_BUF_START);

	ictx->since_ground = evbuffer_new();
	if (ictx->since_ground == NULL)
		fatalx("out of memory");

	evtimer_set(&ictx->timer, input_timer_callback, ictx);

	input_reset(ictx, 0);
	return (ictx);
}

/* Destroy input parser. */
void
input_free(struct input_ctx *ictx)
{
	u_int	i;

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING)
			free(ictx->param_list[i].str);
	}

	event_del(&ictx->timer);

	free(ictx->input_buf);
	evbuffer_free(ictx->since_ground);

	free(ictx);
}

/* Reset input state and clear screen. */
void
input_reset(struct input_ctx *ictx, int clear)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;

	input_reset_cell(ictx);

	if (clear && wp != NULL) {
		if (TAILQ_EMPTY(&wp->modes))
			screen_write_start_pane(sctx, wp, &wp->base);
		else
			screen_write_start(sctx, &wp->base);
		screen_write_reset(sctx);
		screen_write_stop(sctx);
	}

	input_clear(ictx);

	ictx->state = &input_state_ground;
	ictx->flags = 0;
}

/* Return pending data. */
struct evbuffer *
input_pending(struct input_ctx *ictx)
{
	return (ictx->since_ground);
}

/* Change input state. */
static void
input_set_state(struct input_ctx *ictx, const struct input_transition *itr)
{
	if (ictx->state->exit != NULL)
		ictx->state->exit(ictx);
	ictx->state = itr->state;
	if (ictx->state->enter != NULL)
		ictx->state->enter(ictx);
}

/* Parse data. */
static void
input_parse(struct input_ctx *ictx, u_char *buf, size_t len)
{
	struct screen_write_ctx		*sctx = &ictx->ctx;
	const struct input_state	*state = NULL;
	const struct input_transition	*itr = NULL;
	size_t				 off = 0;

	/* Parse the input. */
	while (off < len) {
		ictx->ch = buf[off++];

		/* Find the transition. */
		if (ictx->state != state ||
		    itr == NULL ||
		    ictx->ch < itr->first ||
		    ictx->ch > itr->last) {
			itr = ictx->state->transitions;
			while (itr->first != -1 && itr->last != -1) {
				if (ictx->ch >= itr->first &&
				    ictx->ch <= itr->last)
					break;
				itr++;
			}
			if (itr->first == -1 || itr->last == -1) {
				/* No transition? Eh? */
				fatalx("no transition from state");
			}
		}
		state = ictx->state;

		/*
		 * Any state except print stops the current collection. This is
		 * an optimization to avoid checking if the attributes have
		 * changed for every character. It will stop unnecessarily for
		 * sequences that don't make a terminal change, but they should
		 * be the minority.
		 */
		if (itr->handler != input_print)
			screen_write_collect_end(sctx);

		/*
		 * Execute the handler, if any. Don't switch state if it
		 * returns non-zero.
		 */
		if (itr->handler != NULL && itr->handler(ictx) != 0)
			continue;

		/* And switch state, if necessary. */
		if (itr->state != NULL)
			input_set_state(ictx, itr);

		/* If not in ground state, save input. */
		if (ictx->state != &input_state_ground)
			evbuffer_add(ictx->since_ground, &ictx->ch, 1);
	}
}

/* Parse input from pane. */
void
input_parse_pane(struct window_pane *wp)
{
	void	*new_data;
	size_t	 new_size;

	new_data = window_pane_get_new_data(wp, &wp->offset, &new_size);
	input_parse_buffer(wp, new_data, new_size);
	window_pane_update_used_data(wp, &wp->offset, new_size);
}

/* Parse given input. */
void
input_parse_buffer(struct window_pane *wp, u_char *buf, size_t len)
{
	struct input_ctx	*ictx = wp->ictx;
	struct screen_write_ctx	*sctx = &ictx->ctx;

	if (len == 0)
		return;

	window_update_activity(wp->window);
	wp->flags |= PANE_CHANGED;

	/* Flag new input while in a mode. */
	if (!TAILQ_EMPTY(&wp->modes))
		wp->flags |= PANE_UNSEENCHANGES;

	/* NULL wp if there is a mode set as don't want to update the tty. */
	if (TAILQ_EMPTY(&wp->modes))
		screen_write_start_pane(sctx, wp, &wp->base);
	else
		screen_write_start(sctx, &wp->base);

	log_debug("%s: %%%u %s, %zu bytes: %.*s", __func__, wp->id,
	    ictx->state->name, len, (int)len, buf);

	input_parse(ictx, buf, len);
	screen_write_stop(sctx);
}

/* Parse given input for screen. */
void
input_parse_screen(struct input_ctx *ictx, struct screen *s,
    screen_write_init_ctx_cb cb, void *arg, u_char *buf, size_t len)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;

	if (len == 0)
		return;

	screen_write_start_callback(sctx, s, cb, arg);
	input_parse(ictx, buf, len);
	screen_write_stop(sctx);
}

/* Split the parameter list (if any). */
static int
input_split(struct input_ctx *ictx)
{
	const char		*errstr;
	char			*ptr, *out;
	struct input_param	*ip;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING)
			free(ictx->param_list[i].str);
	}
	ictx->param_list_len = 0;

	if (ictx->param_len == 0)
		return (0);
	ip = &ictx->param_list[0];

	ptr = ictx->param_buf;
	while ((out = strsep(&ptr, ";")) != NULL) {
		if (*out == '\0')
			ip->type = INPUT_MISSING;
		else {
			if (strchr(out, ':') != NULL) {
				ip->type = INPUT_STRING;
				ip->str = xstrdup(out);
			} else {
				ip->type = INPUT_NUMBER;
				ip->num = strtonum(out, 0, INT_MAX, &errstr);
				if (errstr != NULL)
					return (-1);
			}
		}
		ip = &ictx->param_list[++ictx->param_list_len];
		if (ictx->param_list_len == nitems(ictx->param_list))
			return (-1);
	}

	for (i = 0; i < ictx->param_list_len; i++) {
		ip = &ictx->param_list[i];
		if (ip->type == INPUT_MISSING)
			log_debug("parameter %u: missing", i);
		else if (ip->type == INPUT_STRING)
			log_debug("parameter %u: string %s", i, ip->str);
		else if (ip->type == INPUT_NUMBER)
			log_debug("parameter %u: number %d", i, ip->num);
	}

	return (0);
}

/* Get an argument or return default value. */
static int
input_get(struct input_ctx *ictx, u_int validx, int minval, int defval)
{
	struct input_param	*ip;
	int			 retval;

	if (validx >= ictx->param_list_len)
	    return (defval);
	ip = &ictx->param_list[validx];
	if (ip->type == INPUT_MISSING)
		return (defval);
	if (ip->type == INPUT_STRING)
		return (-1);
	retval = ip->num;
	if (retval < minval)
		return (minval);
	return (retval);
}

/* Reply to terminal query. */
static void
input_reply(struct input_ctx *ictx, const char *fmt, ...)
{
	struct bufferevent	*bev = ictx->event;
	va_list			 ap;
	char			*reply;

	if (bev == NULL)
		return;

	va_start(ap, fmt);
	xvasprintf(&reply, fmt, ap);
	va_end(ap);

	log_debug("%s: %s", __func__, reply);
	bufferevent_write(bev, reply, strlen(reply));
	free(reply);
}

/* Clear saved state. */
static void
input_clear(struct input_ctx *ictx)
{
	event_del(&ictx->timer);

	*ictx->interm_buf = '\0';
	ictx->interm_len = 0;

	*ictx->param_buf = '\0';
	ictx->param_len = 0;

	*ictx->input_buf = '\0';
	ictx->input_len = 0;

	ictx->input_end = INPUT_END_ST;

	ictx->flags &= ~INPUT_DISCARD;
}

/* Reset for ground state. */
static void
input_ground(struct input_ctx *ictx)
{
	event_del(&ictx->timer);
	evbuffer_drain(ictx->since_ground, EVBUFFER_LENGTH(ictx->since_ground));

	if (ictx->input_space > INPUT_BUF_START) {
		ictx->input_space = INPUT_BUF_START;
		ictx->input_buf = xrealloc(ictx->input_buf, INPUT_BUF_START);
	}
}

/* Output this character to the screen. */
static int
input_print(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	int			 set;

	input_stop_utf8(ictx); /* can't be valid UTF-8 */

	set = ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set;
	if (set == 1)
		ictx->cell.cell.attr |= GRID_ATTR_CHARSET;
	else
		ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;
	utf8_set(&ictx->cell.cell.data, ictx->ch);
	screen_write_collect_add(sctx, &ictx->cell.cell);

	utf8_copy(&ictx->last, &ictx->cell.cell.data);
	ictx->flags |= INPUT_LAST;

	ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;

	return (0);
}

/* Collect intermediate string. */
static int
input_intermediate(struct input_ctx *ictx)
{
	if (ictx->interm_len == (sizeof ictx->interm_buf) - 1)
		ictx->flags |= INPUT_DISCARD;
	else {
		ictx->interm_buf[ictx->interm_len++] = ictx->ch;
		ictx->interm_buf[ictx->interm_len] = '\0';
	}

	return (0);
}

/* Collect parameter string. */
static int
input_parameter(struct input_ctx *ictx)
{
	if (ictx->param_len == (sizeof ictx->param_buf) - 1)
		ictx->flags |= INPUT_DISCARD;
	else {
		ictx->param_buf[ictx->param_len++] = ictx->ch;
		ictx->param_buf[ictx->param_len] = '\0';
	}

	return (0);
}

/* Collect input string. */
static int
input_input(struct input_ctx *ictx)
{
	size_t available;

	available = ictx->input_space;
	while (ictx->input_len + 1 >= available) {
		available *= 2;
		if (available > input_buffer_size) {
			ictx->flags |= INPUT_DISCARD;
			return (0);
		}
		ictx->input_buf = xrealloc(ictx->input_buf, available);
		ictx->input_space = available;
	}
	ictx->input_buf[ictx->input_len++] = ictx->ch;
	ictx->input_buf[ictx->input_len] = '\0';

	return (0);
}

/* Execute C0 control sequence. */
static int
input_c0_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	struct screen		*s = sctx->s;
	struct grid_cell	 gc, first_gc;
	u_int			 cx, bx, line;
	u_int			 width;
	int			 has_content = 0;

	input_stop_utf8(ictx); /* can't be valid UTF-8 */

	log_debug("%s: '%c'", __func__, ictx->ch);

	switch (ictx->ch) {
	case '\000':	/* NUL */
		break;
	case '\007':	/* BEL */
		if (wp != NULL)
			alerts_queue(wp->window, WINDOW_BELL);
		break;
	case '\010':	/* BS */
		screen_write_backspace(sctx);
		break;
	case '\011':	/* HT */
		/* Don't tab beyond the end of the line. */
		cx = s->cx;
		if (cx >= screen_size_x(s) - 1 || cx == s->rright)
			break;
		if (cx > s->rright)
			bx = screen_size_x(s) - 1;
		else
			bx = s->rright;

		/* Find the next tab point, or use the last column if none. */
		line = s->cy + s->grid->hsize;
		grid_get_cell(s->grid, cx, line, &first_gc);
		do {
			if (!has_content) {
				grid_get_cell(s->grid, cx, line, &gc);
				if (gc.data.size != 1 ||
				    *gc.data.data != ' ' ||
				    !grid_cells_look_equal(&gc, &first_gc))
					has_content = 1;
			}
			cx++;
			if (bit_test(s->tabs, cx))
				break;
		} while (cx < bx);

		width = cx - s->cx;
		if (has_content || width > sizeof gc.data.data)
			screen_write_cursormove(sctx, cx, -1, 0);
		else {
			grid_get_cell(s->grid, s->cx, line, &gc);
			grid_set_tab(&gc, width);
			screen_write_collect_add(sctx, &gc);
		}
		break;
	case '\012':	/* LF */
	case '\013':	/* VT */
	case '\014':	/* FF */
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		if (s->mode & MODE_CRLF)
			screen_write_carriagereturn(sctx);
		break;
	case '\015':	/* CR */
		screen_write_carriagereturn(sctx);
		break;
	case '\016':	/* SO */
		ictx->cell.set = 1;
		break;
	case '\017':	/* SI */
		ictx->cell.set = 0;
		break;
	default:
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		break;
	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Execute escape sequence. */
static int
input_esc_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx		*sctx = &ictx->ctx;
	struct screen			*s = sctx->s;
	struct input_table_entry	*entry;

	if (ictx->flags & INPUT_DISCARD)
		return (0);
	log_debug("%s: '%c', %s", __func__, ictx->ch, ictx->interm_buf);

	entry = bsearch(ictx, input_esc_table, nitems(input_esc_table),
	    sizeof input_esc_table[0], input_table_compare);
	if (entry == NULL) {
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		return (0);
	}

	switch (entry->type) {
	case INPUT_ESC_RIS:
		colour_palette_clear(ictx->palette);
		input_reset_cell(ictx);
		screen_write_reset(sctx);
		screen_write_fullredraw(sctx);
		break;
	case INPUT_ESC_IND:
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_NEL:
		screen_write_carriagereturn(sctx);
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_HTS:
		if (s->cx < screen_size_x(s))
			bit_set(s->tabs, s->cx);
		break;
	case INPUT_ESC_RI:
		screen_write_reverseindex(sctx, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_DECBI:
		if (ictx->term_level < TERM_VT220)
			break;
		screen_write_backindex(sctx, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_DECFI:
		if (ictx->term_level < TERM_VT220)
			break;
		screen_write_forwardindex(sctx, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_DECKPAM:
		screen_write_mode_set(sctx, MODE_KKEYPAD);
		break;
	case INPUT_ESC_DECKPNM:
		screen_write_mode_clear(sctx, MODE_KKEYPAD);
		break;
	case INPUT_ESC_DECSC:
		input_save_state(ictx);
		break;
	case INPUT_ESC_DECRC:
		input_restore_state(ictx);
		break;
	case INPUT_ESC_DECALN:
		screen_write_alignmenttest(sctx);
		break;
	case INPUT_ESC_SCSG0_ON:
		ictx->cell.g0set = 1;
		break;
	case INPUT_ESC_SCSG0_OFF:
		ictx->cell.g0set = 0;
		break;
	case INPUT_ESC_SCSG1_ON:
		ictx->cell.g1set = 1;
		break;
	case INPUT_ESC_SCSG1_OFF:
		ictx->cell.g1set = 0;
		break;
	case INPUT_ESC_ST:
		/* ST terminates OSC but the state transition already did it. */
		break;
	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Execute control sequence. */
static int
input_csi_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx	       *sctx = &ictx->ctx;
	struct screen		       *s = sctx->s;
	struct input_table_entry       *entry;
	int				i, n, m, ek, set;
	u_int				cx, bx, bg = ictx->cell.cell.bg;

	if (ictx->flags & INPUT_DISCARD)
		return (0);

	log_debug("%s: '%c' \"%s\" \"%s\"", __func__, ictx->ch,
	    ictx->interm_buf, ictx->param_buf);

	if (input_split(ictx) != 0)
		return (0);

	entry = bsearch(ictx, input_csi_table, nitems(input_csi_table),
	    sizeof input_csi_table[0], input_table_compare);
	if (entry == NULL) {
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		return (0);
	}

	switch (entry->type) {
	case INPUT_CSI_CBT:
		/* Find the previous tab point, n times. */
		cx = s->cx;
		if (cx > screen_size_x(s) - 1)
			cx = screen_size_x(s) - 1;
		if (cx < s->rleft)
			bx = 0;
		else
			bx = s->rleft;
		n = input_get(ictx, 0, 1, 1);
		if (n == -1)
			break;
		while (cx > bx && n-- > 0) {
			do
				cx--;
			while (cx > bx && !bit_test(s->tabs, cx));
		}
		screen_write_cursormove(sctx, cx, -1, 0);
		break;
	case INPUT_CSI_CHT:
		/* Find the next tab point, n times. */
		cx = s->cx;
		if (cx >= screen_size_x(s) - 1 || cx == s->rright)
			break;
		if (cx > s->rright)
			bx = screen_size_x(s) - 1;
		else
			bx = s->rright;
		n = input_get(ictx, 0, 1, 1);
		if (n == -1)
			break;
		while (cx < bx && n-- > 0) {
			do
				cx++;
			while (cx < bx && !bit_test(s->tabs, cx));
		}
		screen_write_cursormove(sctx, cx, -1, 0);
		break;
	case INPUT_CSI_CUB:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorleft(sctx, n);
		break;
	case INPUT_CSI_CUD:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursordown(sctx, n);
		break;
	case INPUT_CSI_CUF:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorright(sctx, n);
		break;
	case INPUT_CSI_CUP:
		n = input_get(ictx, 0, 1, 1);
		m = input_get(ictx, 1, 1, 1);
		if (n != -1 && m != -1)
			screen_write_cursormove(sctx, m - 1, n - 1, 1);
		break;
	case INPUT_CSI_MODSET:
		n = input_get(ictx, 0, 0, 0);
		if (n != 4)
			break;
		m = input_get(ictx, 1, 0, 0);

		/*
		 * Set the extended key reporting mode as per the client
		 * request, unless "extended-keys" is set to "off".
		 */
		ek = options_get_number(global_options, "extended-keys");
		if (ek == 0)
			break;
		screen_write_mode_clear(sctx, EXTENDED_KEY_MODES);
		if (m == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED_2);
		else if (m == 1 || ek == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED);
		break;
	case INPUT_CSI_MODOFF:
		n = input_get(ictx, 0, 0, 0);
		if (n != 4)
			break;

		/*
		 * Clear the extended key reporting mode as per the client
		 * request, unless "extended-keys always" forces into mode 1.
		 */
		screen_write_mode_clear(sctx,
		    MODE_KEYS_EXTENDED|MODE_KEYS_EXTENDED_2);
		if (options_get_number(global_options, "extended-keys") == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED);
		break;
	case INPUT_CSI_WINOPS:
		input_csi_dispatch_winops(ictx);
		break;
	case INPUT_CSI_CUU:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorup(sctx, n);
		break;
	case INPUT_CSI_CNL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1) {
			screen_write_carriagereturn(sctx);
			screen_write_cursordown(sctx, n);
		}
		break;
	case INPUT_CSI_CPL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1) {
			screen_write_carriagereturn(sctx);
			screen_write_cursorup(sctx, n);
		}
		break;
	case INPUT_CSI_DA:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			switch (ictx->max_level) {
			case TERM_VT125:
#ifdef ENABLE_SIXEL
				input_reply(ictx, "\033[?12;7;0;1c");
				break;
#else
				/* FALLTHROUGH */
#endif
			case TERM_VT100:
				input_reply(ictx, "\033[?1;2c");
				break;
			case TERM_VT101:
				input_reply(ictx, "\033[?1;0c");
				break;
			case TERM_VT102:
				input_reply(ictx, "\033[?6c");
				break;
			case TERM_VT241:
#ifdef ENABLE_SIXEL
				input_reply(ictx, "\033[?62;1;2;4;6;16;17;21;22c");
				break;
#else
				/* FALLTHROUGH */
#endif
			case TERM_VT220:
				input_reply(ictx, "\033[?62;1;2;6;16;17;21;22c");
				break;
			}
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_DA_TWO:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			input_reply(ictx, "\033[>84;0;0c");
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_ECH:
		if (ictx->term_level < TERM_VT220)
			break;
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_clearcharacter(sctx, n, bg);
		break;
	case INPUT_CSI_DCH:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_deletecharacter(sctx, n, bg);
		break;
	case INPUT_CSI_DECSTBM:
		n = input_get(ictx, 0, 1, 1);
		m = input_get(ictx, 1, 1, screen_size_y(s));
		if (n != -1 && m != -1)
			screen_write_scrollregion(sctx, n - 1, m - 1);
		break;
	case INPUT_CSI_DL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_deleteline(sctx, n, bg);
		break;
	case INPUT_CSI_DECDC:
		if (ictx->term_level < TERM_VT220)
			break;
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_deletecolumn(sctx, n, bg);
		break;
	case INPUT_CSI_DSR_PRIVATE:
		switch (input_get(ictx, 0, 0, 0)) {
		case 996:
			input_report_current_theme(ictx);
			break;
		}
		break;
	case INPUT_CSI_DSR:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 5:
			input_reply(ictx, "\033[0n");
			break;
		case 6:
			input_reply(ictx, "\033[%u;%uR",
			    s->cy + 1 - ((s->mode & MODE_ORIGIN) ? s->rupper : 0),
			    s->cx + 1 - ((s->mode & MODE_ORIGIN) ? s->rleft : 0));
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_ED:
	case INPUT_CSI_DECSED:
		m = entry->type == INPUT_CSI_DECSED;
		if (m && ictx->term_level < TERM_VT220)
			break;
		switch ((n = input_get(ictx, 0, 0, 0))) {
		case -1:
			break;
		case 0:
			screen_write_clearendofscreen(sctx, bg, m);
			break;
		case 1:
			screen_write_clearstartofscreen(sctx, bg, m);
			break;
		case 2:
			screen_write_clearscreen(sctx, bg, m);
			break;
		case 3:
			if (input_get(ictx, 1, 0, 0) == 0) {
				/*
				 * Linux console extension to clear history
				 * (for example before locking the screen).
				 */
				screen_write_clearhistory(sctx, m);
			}
			break;
		default:
			log_debug("%s: unknown erase display %d", __func__, n);
			break;
		}
		break;
	case INPUT_CSI_EL:
	case INPUT_CSI_DECSEL:
		m = entry->type == INPUT_CSI_DECSEL;
		if (m && ictx->term_level < TERM_VT220)
			break;
		switch ((n = input_get(ictx, 0, 0, 0))) {
		case -1:
			break;
		case 0:
			screen_write_clearendofline(sctx, bg, m);
			break;
		case 1:
			screen_write_clearstartofline(sctx, bg, m);
			break;
		case 2:
			screen_write_clearline(sctx, bg, m);
			break;
		default:
			log_debug("%s: unknown erase line %d", __func__, n);
			break;
		}
		break;
	case INPUT_CSI_HPA:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursormove(sctx, n - 1, -1, 1);
		break;
	case INPUT_CSI_ICH:
		if (ictx->term_level < TERM_VT220)
			break;
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_insertcharacter(sctx, n, bg);
		break;
	case INPUT_CSI_IL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_insertline(sctx, n, bg);
		break;
	case INPUT_CSI_DECIC:
		if (ictx->term_level < TERM_VT220)
			break;
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_insertcolumn(sctx, n, bg);
		break;
	case INPUT_CSI_REP:
		n = input_get(ictx, 0, 1, 1);
		if (n == -1)
			break;

		m = screen_size_x(s) - s->cx;
		if (n > m)
			n = m;

		if (~ictx->flags & INPUT_LAST)
			break;

		set = ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set;
		if (set == 1)
			ictx->cell.cell.attr |= GRID_ATTR_CHARSET;
		else
			ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;
		utf8_copy(&ictx->cell.cell.data, &ictx->last);
		for (i = 0; i < n; i++)
			screen_write_collect_add(sctx, &ictx->cell.cell);
		break;
	case INPUT_CSI_RCP:
		input_restore_state(ictx);
		break;
	case INPUT_CSI_RM:
		input_csi_dispatch_rm(ictx);
		break;
	case INPUT_CSI_RM_PRIVATE:
		input_csi_dispatch_rm_private(ictx);
		break;
	case INPUT_CSI_SCP_DECSLRM:
		if (s->mode & MODE_LR_MARGINS) {
			n = input_get(ictx, 0, 1, 1);
			m = input_get(ictx, 1, 1, screen_size_x(s));
			if (n != -1 && m != -1)
				screen_write_scrollmargin(sctx, n - 1, m - 1);
		} else
			input_save_state(ictx);
		break;
	case INPUT_CSI_SGR:
		input_csi_dispatch_sgr(ictx);
		break;
	case INPUT_CSI_SM:
		input_csi_dispatch_sm(ictx);
		break;
	case INPUT_CSI_SM_PRIVATE:
		input_csi_dispatch_sm_private(ictx);
		break;
	case INPUT_CSI_SM_GRAPHICS:
		input_csi_dispatch_sm_graphics(ictx);
		break;
	case INPUT_CSI_SU:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrollup(sctx, n, bg);
		break;
	case INPUT_CSI_SD:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrolldown(sctx, n, bg);
		break;
	case INPUT_CSI_SL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrollleft(sctx, n, bg);
		break;
	case INPUT_CSI_SR:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrollright(sctx, n, bg);
		break;
	case INPUT_CSI_TBC:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			if (s->cx < screen_size_x(s))
				bit_clear(s->tabs, s->cx);
			break;
		case 3:
			bit_nclear(s->tabs, 0, screen_size_x(s) - 1);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_VPA:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursormove(sctx, -1, n - 1, 1);
		break;
	case INPUT_CSI_DECSCUSR:
		n = input_get(ictx, 0, 0, 0);
		if (n == -1)
			break;
		screen_set_cursor_style(n, &s->cstyle, &s->mode);
		if (n == 0) {
			/* Go back to default blinking state. */
			screen_write_mode_clear(sctx, MODE_CURSOR_BLINKING_SET);
		}
		break;
	case INPUT_CSI_XDA:
		n = input_get(ictx, 0, 0, 0);
		if (n == 0)
			input_reply(ictx, "\033P>|tmux %s\033\\", getversion());
		break;
	case INPUT_CSI_DECRQM:
		if (ictx->term_level >= TERM_VT220)
			input_csi_dispatch_decrqm(ictx);
		break;
	case INPUT_CSI_DECRQM_PRIVATE:
		if (ictx->term_level >= TERM_VT220)
			input_csi_dispatch_decrqm_private(ictx);
		break;
	case INPUT_CSI_DECRQPSR:
		if (ictx->term_level >= TERM_VT220)
			input_csi_dispatch_decrqpsr(ictx);
		break;
	case INPUT_CSI_DECRQTSR:
		if (ictx->term_level >= TERM_VT220)
			input_csi_dispatch_decrqtsr(ictx);
		break;
	case INPUT_CSI_DECSCL:
		if (ictx->max_level < TERM_VT220)
			break;
		m = input_get(ictx, 1, 0, 0);
		switch ((n = input_get(ictx, 0, 61, 0))) {
		case -1:
			break;
		case 61:
#ifdef ENABLE_SIXEL
			ictx->term_level = input_is_graphics_term(
			    ictx->max_level) ? TERM_VT125 : TERM_VT100;
#else
			ictx->term_level = TERM_VT100;
#endif
			log_debug("%s: switching to level 1", __func__);
			input_soft_reset(ictx);
			break;
		case 62:
			if (m != 1) {
				log_debug("%s: 8-bit mode is not yet supported",
				    __func__);
				break;
			}
#ifdef ENABLE_SIXEL
			ictx->term_level = input_is_graphics_term(
			    ictx->max_level) ? TERM_VT241 : TERM_VT220;
#else
			ictx->term_level = TERM_VT220;
#endif
			log_debug("%s: switching to level 2", __func__);
			input_soft_reset(ictx);
			break;
		default:
			log_debug("%s: unhandled level %d", __func__, n);
			break;
		}
		break;
	case INPUT_CSI_DECSTR:
		if (ictx->term_level >= TERM_VT220)
			input_soft_reset(ictx);
		break;
	case INPUT_CSI_DECSCA:
		if (ictx->term_level < TERM_VT220)
			break;
		switch ((n = input_get(ictx, 0, 0, 0))) {
		case -1:
			break;
		case 0:
		case 2:
			ictx->cell.cell.attr &= ~GRID_ATTR_PROTECTED;
			break;
		case 1:
			ictx->cell.cell.attr |= GRID_ATTR_PROTECTED;
			break;
		default:
			log_debug("%s: unknown DECSCA %d", __func__, n);
			break;
		}
		break;

	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Handle CSI RM. */
static void
input_csi_dispatch_rm(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 4:		/* IRM */
			screen_write_mode_clear(sctx, MODE_INSERT);
			break;
		case 20:	/* LNM */
			screen_write_mode_clear(sctx, MODE_CRLF);
			break;
		case 34:	/* SCSTCURM */
			screen_write_mode_set(sctx, MODE_CURSOR_VERY_VISIBLE);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI DECRST (private RM). */
static void
input_csi_dispatch_rm_private(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 1:		/* DECCKM */
			screen_write_mode_clear(sctx, MODE_KCURSOR);
			break;
		case 3:		/* DECCOLM */
			screen_write_cursormove(sctx, 0, 0, 1);
			screen_write_clearscreen(sctx, gc->bg, 0);
			break;
		case 6:		/* DECOM */
			screen_write_mode_clear(sctx, MODE_ORIGIN);
			screen_write_cursormove(sctx, 0, 0, 1);
			break;
		case 7:		/* DECAWM */
			screen_write_mode_clear(sctx, MODE_WRAP);
			break;
		case 12:	/* ATTCUBL */
			screen_write_mode_clear(sctx, MODE_CURSOR_BLINKING);
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING_SET);
			break;
		case 25:	/* DECTCEM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECTCEM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_clear(sctx, MODE_CURSOR);
			break;
		case 66:	/* DECNKM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECNKM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_clear(sctx, MODE_KKEYPAD);
			break;
		case 69:	/* DECLRMM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECLRMM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_clear(sctx, MODE_LR_MARGINS);
			screen_write_scrollmargin(sctx, 0, screen_size_x(sctx->s) - 1);
			break;
		case 1000:	/* XT_MSE_X11 */
		case 1001:	/* XT_MSE_HL */
		case 1002:	/* XT_MSE_BTN */
		case 1003:	/* XT_MSE_ANY */
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			break;
		case 1004:	/* XT_MSE_WIN */
			screen_write_mode_clear(sctx, MODE_FOCUSON);
			break;
		case 1005:	/* XT_MSE_UTF */
			screen_write_mode_clear(sctx, MODE_MOUSE_UTF8);
			break;
		case 1006:	/* XT_MSE_SGR */
			screen_write_mode_clear(sctx, MODE_MOUSE_SGR);
			break;
		case 47:	/* XT_ALTSCRN */
		case 1047:	/* XT_ALTS_47 */
			screen_write_alternateoff(sctx, gc, 0);
			break;
		case 1049:	/* XT_EXTSCRN */
			screen_write_alternateoff(sctx, gc, 1);
			break;
		case 2004:	/* RL_BRACKET */
			screen_write_mode_clear(sctx, MODE_BRACKETPASTE);
			break;
		case 2031:
			screen_write_mode_clear(sctx, MODE_THEME_UPDATES);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI SM. */
static void
input_csi_dispatch_sm(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 4:		/* IRM */
			screen_write_mode_set(sctx, MODE_INSERT);
			break;
		case 20:	/* LNM */
			screen_write_mode_set(sctx, MODE_CRLF);
			break;
		case 34:	/* SCSTCURM */
			screen_write_mode_clear(sctx, MODE_CURSOR_VERY_VISIBLE);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI DECSET (private SM). */
static void
input_csi_dispatch_sm_private(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 1:		/* DECCKM */
			screen_write_mode_set(sctx, MODE_KCURSOR);
			break;
		case 3:		/* DECCOLM */
			screen_write_cursormove(sctx, 0, 0, 1);
			screen_write_clearscreen(sctx, ictx->cell.cell.bg, 0);
			break;
		case 6:		/* DECOM */
			screen_write_mode_set(sctx, MODE_ORIGIN);
			screen_write_cursormove(sctx, 0, 0, 1);
			break;
		case 7:		/* DECAWM */
			screen_write_mode_set(sctx, MODE_WRAP);
			break;
		case 12:	/* ATTCUBL */
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING);
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING_SET);
			break;
		case 25:	/* DECTCEM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECTCEM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_set(sctx, MODE_CURSOR);
			break;
		case 66:	/* DECNKM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECNKM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_set(sctx, MODE_KKEYPAD);
			break;
		case 69:	/* DECLRMM */
			if (ictx->term_level < TERM_VT220) {
				log_debug("%s: DECLRMM ignored in VT100 mode",
				    __func__);
				break;
			}
			screen_write_mode_set(sctx, MODE_LR_MARGINS);
			break;
		case 1000:	/* XT_MSE_X11 */
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_STANDARD);
			break;
		case 1002:	/* XT_MSE_BTN */
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_BUTTON);
			break;
		case 1003:	/* XT_MSE_ANY */
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_ALL);
			break;
		case 1004:	/* XT_MSE_WIN */
			screen_write_mode_set(sctx, MODE_FOCUSON);
			break;
		case 1005:	/* XT_MSE_UTF */
			screen_write_mode_set(sctx, MODE_MOUSE_UTF8);
			break;
		case 1006:	/* XT_MSE_SGR */
			screen_write_mode_set(sctx, MODE_MOUSE_SGR);
			break;
		case 47:	/* XT_ALTSCRN */
		case 1047:	/* XT_ALTS_47 */
			screen_write_alternateon(sctx, gc, 0);
			break;
		case 1049:	/* XT_EXTSCRN */
			screen_write_alternateon(sctx, gc, 1);
			break;
		case 2004:	/* RL_BRACKET */
			screen_write_mode_set(sctx, MODE_BRACKETPASTE);
			break;
		case 2031:
			screen_write_mode_set(sctx, MODE_THEME_UPDATES);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI graphics SM. */
static void
input_csi_dispatch_sm_graphics(__unused struct input_ctx *ictx)
{
#ifdef ENABLE_SIXEL
	int	n, m, o;

	if (!input_is_graphics_term(ictx->term_level))
		return;
	if (ictx->param_list_len > 3)
		return;
	n = input_get(ictx, 0, 0, 0);
	m = input_get(ictx, 1, 0, 0);
	o = input_get(ictx, 2, 0, 0);

	if (n == 1 && (m == 1 || m == 2 || m == 4))
		input_reply(ictx, "\033[?%d;0;%uS", n, SIXEL_COLOUR_REGISTERS);
	else
		input_reply(ictx, "\033[?%d;3;%dS", n, o);
#endif
}

/* Handle CSI DECRQM (ANSI modes). */
static void
input_csi_dispatch_decrqm(struct input_ctx *ictx)
{
	struct screen	*s = ictx->ctx.s;
	int		 m, v;

	m = input_get(ictx, 0, 0, -1);
	switch (m) {
	case -1:
		return;
	case 1:		/* GATM */
	case 5:		/* SRTM */
	case 6:		/* ERM */
	case 7:		/* VEM */
	case 8:		/* BDSM */
	case 9:		/* DCSM */
	case 10:	/* HEM */
	case 11:	/* PUM */
	case 13:	/* FEAM */
	case 14:	/* FETM */
	case 15:	/* MATM */
	case 16:	/* TTM */
	case 17:	/* SATM */
	case 18:	/* TSM */
	case 19:	/* EBM */
	case 21:	/* GRCM */
	case 22:	/* ZDM */
	case 2:		/* KAM */
	case 3:		/* CRM */
	case 12:	/* SRM */
		v = 4;
		break;
	case 4:		/* IRM */
		v = !(s->mode & MODE_INSERT) + 1;
		break;
	case 20:	/* LNM */
		v = !(s->mode & MODE_CRLF) + 1;
		break;
	case 34:	/* SCSTCURM */
		v = !!(s->mode & MODE_CURSOR_VERY_VISIBLE) + 1;
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		v = 0;
		break;
	}
	log_debug("%s: reporting %d for mode %d", __func__, v, m);
	input_reply(ictx, "\033[%d;%d$y", m, v);
}

/* Handle CSI DECRQM (private modes). */
static void
input_csi_dispatch_decrqm_private(struct input_ctx *ictx)
{
	struct screen	*s = ictx->ctx.s;
	struct options	*oo;
	int		 m, v, p;

	m = input_get(ictx, 0, 0, -1);
	switch (m) {
	case -1:
		return;
	case 1:		/* DECCKM */
		v = !(s->mode & MODE_KCURSOR) + 1;
		break;
	case 2:		/* DECANM */
		/* No VT52 mode here */
		v = 3;
		break;
	case 3:		/* DECCOLM */
		/* Not really supported here */
		v = 4;
		break;
	case 4:		/* DECSCLM */
	case 5:		/* DECSCNM */
		/* Not supported */
		v = 4;
		break;
	case 6:		/* DECOM */
		v = !(s->mode & MODE_ORIGIN) + 1;
		break;
	case 7:		/* DECAWM */
		v = !(s->mode & MODE_WRAP) + 1;
		break;
	case 8:		/* DECARM */
		/* Really depends on the client */
		v = 3;
		break;
	case 12:	/* ATTCUBL */
	case 13:	/* XT_OPTBLNK */
		/* cursor blink: 1 = blink, 2 = steady */
		if (s->cstyle != SCREEN_CURSOR_DEFAULT ||
		    s->mode & MODE_CURSOR_BLINKING_SET)
			v = !(s->mode & MODE_CURSOR_BLINKING) + 1;
		else {
			if (ictx->wp != NULL)
				oo = ictx->wp->options;
			else
				oo = global_options;
			p = options_get_number(oo, "cursor-style");

			/* blink for 1,3,5; steady for 0,2,4,6 */
			v = (p == 1 || p == 3 || p == 5) ? 1 : 2;
		}
		break;
	case 14:	/* XT_XORBLNK */
		/* 3 = XT_OPTBLNK and ATTCUBL XORed; 4 = inclusive OR */
		v = 4;
		break;
	case 18:	/* DECPFF */
	case 19:	/* DECPFX */
		/* Not supported */
		v = 4;
		break;
	case 25:	/* DECTCEM */
		v = !(s->mode & MODE_CURSOR) + 1;
		break;
	case 66:	/* DECNKM */
		v = !(s->mode & MODE_KKEYPAD) + 1;
		break;
	case 69:	/* DECLRMM */
		v = !(s->mode & MODE_LR_MARGINS) + 1;
		break;
	case 1000:	/* XT_MSE_X11 */
		v = !(s->mode & MODE_MOUSE_STANDARD) + 1;
		break;
	case 1001:	/* XT_MSE_HL */
		/* Not supported */
		v = 4;
		break;
	case 1002:	/* XT_MSE_BTN */
		v = !(s->mode & MODE_MOUSE_BUTTON) + 1;
		break;
	case 1003:	/* XT_MSE_ALL */
		v = !(s->mode & MODE_MOUSE_ALL) + 1;
		break;
	case 1004:	/* XT_MSE_WIN - focus reporting */
		v = !(s->mode & MODE_FOCUSON) + 1;
		break;
	case 1005:	/* XT_MSE_UTF - urxvt mouse */
		v = !(s->mode & MODE_MOUSE_UTF8) + 1;
		break;
	case 1006:	/* XT_MSE_SGR - SGR mouse */
		v = !(s->mode & MODE_MOUSE_SGR) + 1;
		break;
	case 47:	/* XT_ALTSCRN */
	case 1047:	/* XT_ALTS_47 */
	case 1049:	/* XT_EXTSCRN */
		v = !s->saved_grid + 1;
		break;
	case 2004:	/* RL_BRACKET - bracketed paste */
		v = !(s->mode & MODE_BRACKETPASTE) + 1;
		break;
	case 2031:
		v = !(s->mode & MODE_THEME_UPDATES) + 1;
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		v = 0;
		break;
	}
	log_debug("%s: reporting %d for mode %d", __func__, v, m);
	input_reply(ictx, "\033[?%d;%d$y", m, v);
}

/* Handle CSI window operations. */
static void
input_csi_dispatch_winops(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct screen		*s = sctx->s;
	struct window_pane	*wp = ictx->wp;
	struct window		*w = NULL;
	u_int			 x = screen_size_x(s), y = screen_size_y(s);
	int			 n, m;

	if (wp != NULL)
		w = wp->window;

	m = 0;
	while ((n = input_get(ictx, m, 0, -1)) != -1) {
		switch (n) {
		case 1:
		case 2:
		case 5:
		case 6:
		case 7:
		case 11:
		case 13:
		case 20:
		case 21:
		case 24:
			break;
		case 3:
		case 4:
		case 8:
			m++;
			if (input_get(ictx, m, 0, -1) == -1)
				return;
			/* FALLTHROUGH */
		case 9:
		case 10:
			m++;
			if (input_get(ictx, m, 0, -1) == -1)
				return;
			break;
		case 14:
			if (w == NULL)
				break;
			input_reply(ictx, "\033[4;%u;%ut", y * w->ypixel,
			    x * w->xpixel);
			break;
		case 15:
			if (w == NULL)
				break;
			input_reply(ictx, "\033[5;%u;%ut", y * w->ypixel,
			    x * w->xpixel);
			break;
		case 16:
			if (w == NULL)
				break;
			input_reply(ictx, "\033[6;%u;%ut", w->ypixel,
			    w->xpixel);
			break;
		case 18:
			input_reply(ictx, "\033[8;%u;%ut", y, x);
			break;
		case 19:
			input_reply(ictx, "\033[9;%u;%ut", y, x);
			break;
		case 22:
			m++;
			switch (input_get(ictx, m, 0, -1)) {
			case -1:
				return;
			case 0:
			case 2:
				screen_push_title(sctx->s);
				break;
			}
			break;
		case 23:
			m++;
			switch (input_get(ictx, m, 0, -1)) {
			case -1:
				return;
			case 0:
			case 2:
				screen_pop_title(sctx->s);
				if (wp == NULL)
					break;
				notify_pane("pane-title-changed", wp);
				server_redraw_window_borders(w);
				server_status_window(w);
				break;
			}
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		m++;
	}
}

/* Helper for 256 colour SGR. */
static int
input_csi_dispatch_sgr_256_do(struct input_ctx *ictx, int fgbg, int c)
{
	struct grid_cell	*gc = &ictx->cell.cell;

	if (c == -1 || c > 255) {
		if (fgbg == 38)
			gc->fg = 8;
		else if (fgbg == 48)
			gc->bg = 8;
	} else {
		if (fgbg == 38)
			gc->fg = c | COLOUR_FLAG_256;
		else if (fgbg == 48)
			gc->bg = c | COLOUR_FLAG_256;
		else if (fgbg == 58)
			gc->us = c | COLOUR_FLAG_256;
	}
	return (1);
}

/* Handle CSI SGR for 256 colours. */
static void
input_csi_dispatch_sgr_256(struct input_ctx *ictx, int fgbg, u_int *i)
{
	int	c;

	c = input_get(ictx, (*i) + 1, 0, -1);
	if (input_csi_dispatch_sgr_256_do(ictx, fgbg, c))
		(*i)++;
}

/* Helper for RGB colour SGR. */
static int
input_csi_dispatch_sgr_rgb_do(struct input_ctx *ictx, int fgbg, int r, int g,
    int b)
{
	struct grid_cell	*gc = &ictx->cell.cell;

	if (r == -1 || r > 255)
		return (0);
	if (g == -1 || g > 255)
		return (0);
	if (b == -1 || b > 255)
		return (0);

	if (fgbg == 38)
		gc->fg = colour_join_rgb(r, g, b);
	else if (fgbg == 48)
		gc->bg = colour_join_rgb(r, g, b);
	else if (fgbg == 58)
		gc->us = colour_join_rgb(r, g, b);
	return (1);
}

/* Handle CSI SGR for RGB colours. */
static void
input_csi_dispatch_sgr_rgb(struct input_ctx *ictx, int fgbg, u_int *i)
{
	int	r, g, b;

	r = input_get(ictx, (*i) + 1, 0, -1);
	g = input_get(ictx, (*i) + 2, 0, -1);
	b = input_get(ictx, (*i) + 3, 0, -1);
	if (input_csi_dispatch_sgr_rgb_do(ictx, fgbg, r, g, b))
		(*i) += 3;
}

/* Handle CSI SGR with a ISO parameter. */
static void
input_csi_dispatch_sgr_colon(struct input_ctx *ictx, u_int i)
{
	struct grid_cell	*gc = &ictx->cell.cell;
	char			*s = ictx->param_list[i].str, *copy, *ptr, *out;
	int			 p[8];
	u_int			 n;
	const char		*errstr;

	for (n = 0; n < nitems(p); n++)
		p[n] = -1;
	n = 0;

	ptr = copy = xstrdup(s);
	while ((out = strsep(&ptr, ":")) != NULL) {
		if (*out != '\0') {
			p[n++] = strtonum(out, 0, INT_MAX, &errstr);
			if (errstr != NULL || n == nitems(p)) {
				free(copy);
				return;
			}
		} else {
			n++;
			if (n == nitems(p)) {
				free(copy);
				return;
			}
		}
		log_debug("%s: %u = %d", __func__, n - 1, p[n - 1]);
	}
	free(copy);

	if (n == 0)
		return;
	if (p[0] == 4) {
		if (n != 2)
			return;
		switch (p[1]) {
		case 0:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			break;
		case 1:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE;
			break;
		case 2:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_2;
			break;
		case 3:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_3;
			break;
		case 4:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_4;
			break;
		case 5:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_5;
			break;
		}
		return;
	}
	if (n < 2 || (p[0] != 38 && p[0] != 48 && p[0] != 58))
		return;
	switch (p[1]) {
	case 2:
		if (n < 3)
			break;
		if (n == 5)
			i = 2;
		else
			i = 3;
		if (n < i + 3)
			break;
		input_csi_dispatch_sgr_rgb_do(ictx, p[0], p[i], p[i + 1],
		    p[i + 2]);
		break;
	case 5:
		if (n < 3)
			break;
		input_csi_dispatch_sgr_256_do(ictx, p[0], p[2]);
		break;
	}
}

/* Handle CSI SGR. */
static void
input_csi_dispatch_sgr(struct input_ctx *ictx)
{
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i, link;
	int			 n;

	if (ictx->param_list_len == 0) {
		memcpy(gc, &grid_default_cell, sizeof *gc);
		return;
	}

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING) {
			input_csi_dispatch_sgr_colon(ictx, i);
			continue;
		}
		n = input_get(ictx, i, 0, 0);
		if (n == -1)
			continue;

		if (n == 38 || n == 48 || n == 58) {
			i++;
			switch (input_get(ictx, i, 0, -1)) {
			case 2:
				input_csi_dispatch_sgr_rgb(ictx, n, &i);
				break;
			case 5:
				input_csi_dispatch_sgr_256(ictx, n, &i);
				break;
			}
			continue;
		}

		switch (n) {
		case 0:
			link = gc->link;
			memcpy(gc, &grid_default_cell, sizeof *gc);
			gc->link = link;
			break;
		case 1:
			gc->attr |= GRID_ATTR_BRIGHT;
			break;
		case 2:
			gc->attr |= GRID_ATTR_DIM;
			break;
		case 3:
			gc->attr |= GRID_ATTR_ITALICS;
			break;
		case 4:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE;
			break;
		case 5:
		case 6:
			gc->attr |= GRID_ATTR_BLINK;
			break;
		case 7:
			gc->attr |= GRID_ATTR_REVERSE;
			break;
		case 8:
			gc->attr |= GRID_ATTR_HIDDEN;
			break;
		case 9:
			gc->attr |= GRID_ATTR_STRIKETHROUGH;
			break;
		case 21:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_2;
			break;
		case 22:
			gc->attr &= ~(GRID_ATTR_BRIGHT|GRID_ATTR_DIM);
			break;
		case 23:
			gc->attr &= ~GRID_ATTR_ITALICS;
			break;
		case 24:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			break;
		case 25:
			gc->attr &= ~GRID_ATTR_BLINK;
			break;
		case 27:
			gc->attr &= ~GRID_ATTR_REVERSE;
			break;
		case 28:
			gc->attr &= ~GRID_ATTR_HIDDEN;
			break;
		case 29:
			gc->attr &= ~GRID_ATTR_STRIKETHROUGH;
			break;
		case 30:
		case 31:
		case 32:
		case 33:
		case 34:
		case 35:
		case 36:
		case 37:
			gc->fg = n - 30;
			break;
		case 39:
			gc->fg = 8;
			break;
		case 40:
		case 41:
		case 42:
		case 43:
		case 44:
		case 45:
		case 46:
		case 47:
			gc->bg = n - 40;
			break;
		case 49:
			gc->bg = 8;
			break;
		case 53:
			gc->attr |= GRID_ATTR_OVERLINE;
			break;
		case 55:
			gc->attr &= ~GRID_ATTR_OVERLINE;
			break;
		case 59:
			gc->us = 8;
			break;
		case 90:
		case 91:
		case 92:
		case 93:
		case 94:
		case 95:
		case 96:
		case 97:
			gc->fg = n;
			break;
		case 100:
		case 101:
		case 102:
		case 103:
		case 104:
		case 105:
		case 106:
		case 107:
			gc->bg = n - 10;
			break;
		}
	}
}

/* Handle CSI DECRQPSR. */
static void
input_csi_dispatch_decrqpsr(struct input_ctx *ictx)
{
	int	m;

	m = input_get(ictx, 0, 0, 0);
	switch (m) {
	case -1:
		break;
	case 1:	/* DECCIR */
		input_reply_deccir(ictx);
		break;
	case 2:	/* DECTABSR */
		input_reply_dectabsr(ictx);
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		break;
	}
}

/* Reply to DECRQPSR with a DCS DECCIR. */
static void
input_reply_deccir(struct input_ctx *ictx)
{
	struct screen		*s = ictx->ctx.s;
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 cx, cy, pg = 1, gl = ictx->cell.set, gr = 0;
	char			 sgr = '@', sca = '@', mode = '@', css = '@';
	const char		*g0, *g1, *g2 = "B", *g3 = "B";

	cx = s->cx + 1;
	if (s->mode & MODE_ORIGIN)
		cx -= s->rleft;
	cy = s->cy + 1;
	if (s->mode & MODE_ORIGIN)
		cy -= s->rupper;
	if (gc->attr & GRID_ATTR_BRIGHT)
		sgr |= 0x01;
	if (gc->attr & GRID_ATTR_ALL_UNDERSCORE)
		sgr |= 0x02;
	if (gc->attr & GRID_ATTR_BLINK)
		sgr |= 0x04;
	if (gc->attr & GRID_ATTR_REVERSE)
		sgr |= 0x08;
	if (gc->attr & GRID_ATTR_PROTECTED)
		sca |= 0x01;
	if (s->mode & MODE_ORIGIN)
		mode |= 0x01;
	if (s->cx == s->rright + 1) {
		mode |= 0x08;	/* Last Column Flag (implicit in tmux) */
		cx--;
	}
	g0 = ictx->cell.g0set ? "0" : "B";
	g1 = ictx->cell.g1set ? "0" : "B";

	log_debug("%s: cursor (%u,%u,%u) SGR=%c DECSCA=%c mode=%c", __func__,
	    s->cx, s->cy, pg, sgr, sca, mode);
	log_debug("%s: GL=G%u GR=G%u css=%c G0=%s G1=%s G2=%s G3=%s", __func__,
	    gl, gr, css, g0, g1, g2, g3);
	input_reply(ictx, "\033P1$u%u;%u;%u;%c;%c;%c;%u;%u;%c;%s%s%s%s\033\\",
	    cy, cx, pg, sgr, sca, mode, gl, gr, css, g0, g1, g2, g3);
}

/* Reply to DECRQPSR with a DCS DECTABSR. */
static void
input_reply_dectabsr(struct input_ctx *ictx)
{
	struct bufferevent	*bev = ictx->event;
	struct screen		*s = ictx->ctx.s;
	u_int			 xx, n = 0;
	char			*reply;
	int			 len;

	if (bev == NULL)
		return;

	bufferevent_write(bev, "\033P2$u", 5);	/* DECPSR: DECTABSR */
	for (xx = 0; xx < screen_size_x(s); ++xx) {
		if (bit_test(s->tabs, xx)) {
			log_debug("%s: tab stop at %u", __func__, xx);
			len = xasprintf(&reply, "%s%u", n++ > 0 ? "/" : "",
			    xx + 1);
			bufferevent_write(bev, reply, len);
			free(reply);
		}
	}
	bufferevent_write(bev, "\033\\", 2);	/* ST */
}

/* Handle CSI DECRQTSR. */
static void
input_csi_dispatch_decrqtsr(struct input_ctx *ictx)
{
	int	m;

	m = input_get(ictx, 0, 0, 0);
	switch (m) {
	case -1:
		break;
	case 1:	/* DECTSR */
		/* Not really supported ATM. */
		input_reply(ictx, "\033P1$s\033\\");
		break;
	case 2:	/* DECCTR */
		input_reply_decctr(ictx);
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		break;
	}
}

/* Reply to DECRQTSR with a DCS DECCTR. */
static void
input_reply_decctr(struct input_ctx *ictx)
{
	struct bufferevent	*bev = ictx->event;
	char			*reply;
	int			 len, cs, i, c;
	u_char			 r, g, b, l, s;
	u_short			 h;

	if (bev == NULL)
		return;

	cs = input_get(ictx, 1, 0, 2);
	if (cs == -1)
		return;
	if (cs > 2) {
		log_debug("%s: unknown color space %d", __func__, cs);
		return;
	}
	if (cs == 0)
		cs = 2;

	bufferevent_write(bev, "\033P2$s", 5);	/* DECTSR: DECCTR */
	for (i = 0; i < 256; ++i) {
		c = colour_palette_get(ictx->palette, i | COLOUR_FLAG_256);
		if (c != -1)
			c = colour_force_rgb(c);
		if (c == -1) {
			log_debug("%s: colour %d invalid", __func__, i);
			continue;
		}
		switch (cs) {
		case 1:
			colour_split_hls(c, &h, &l, &s);
			len = xasprintf(&reply, "%s%d;%d;%hu;%hhu;%hhu",
			    i > 0 ? "/" : "", i, cs, h, l, s);
			break;
		case 2:
			colour_split_rgb(c, &r, &g, &b);
			/*
			 * DECCTR reports RGB colours from 0-100 instead of
			 * 0-255...
			 */
			r = r * 100 / 255;
			g = g * 100 / 255;
			b = b * 100 / 255;
			len = xasprintf(&reply, "%s%d;%d;%hhu;%hhu;%hhu",
			    i > 0 ? "/" : "", i, cs, r, g, b);
			break;
		}
		bufferevent_write(bev, reply, len);
		free(reply);
	}
	bufferevent_write(bev, "\033\\", 2);	/* ST */
}

/* End of input with BEL. */
static int
input_end_bel(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	ictx->input_end = INPUT_END_BEL;

	return (0);
}

/* DCS string started. */
static void
input_enter_dcs(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* DCS terminator (ST) received. */
static int
input_dcs_dispatch(struct input_ctx *ictx)
{
	struct window_pane		*wp = ictx->wp;
	struct options			*oo;
	struct screen_write_ctx		*sctx = &ictx->ctx;
	struct input_table_entry	*entry;
	u_char				*buf = ictx->input_buf;
	size_t				 len = ictx->input_len;
	const char			 prefix[] = "tmux;";
	const u_int			 prefixlen = (sizeof prefix) - 1;
	long long			 allow_passthrough = 0;
#ifdef ENABLE_SIXEL
	struct window			*w;
	struct sixel_image		*si;
	int				 p2;
#endif

	if (wp == NULL)
		return (0);
	oo = wp->options;

	if (ictx->flags & INPUT_DISCARD) {
		log_debug("%s: %zu bytes (discard)", __func__, len);
		return (0);
	}

	log_debug("%s: \"%s\" \"%s\" \"%s\"", __func__, buf, ictx->interm_buf,
	    ictx->param_buf);

	allow_passthrough = options_get_number(oo, "allow-passthrough");
	if (allow_passthrough && len >= prefixlen &&
	    strncmp(buf, prefix, prefixlen) == 0) {
		screen_write_rawstring(sctx, buf + prefixlen, len - prefixlen,
		    allow_passthrough == 2);
		return (0);
	}

	if (input_split(ictx) != 0)
		return (0);
	ictx->ch = buf[0];

	entry = bsearch(ictx, input_dcs_table, nitems(input_dcs_table),
	    sizeof input_dcs_table[0], input_table_compare);
	if (entry == NULL) {
		log_debug("%s: unknown \"%s%c\"", __func__, ictx->interm_buf,
		    *buf);
		return (0);
	}

	switch (entry->type) {
	case INPUT_DCS_DECRQSS:
		if (ictx->term_level >= TERM_VT220)
			input_dcs_dispatch_decrqss(ictx);
		break;
	case INPUT_DCS_DECRSPS:
		if (ictx->term_level >= TERM_VT220)
			input_dcs_dispatch_decrsps(ictx);
		break;
	case INPUT_DCS_DECRSTS:
		if (ictx->term_level >= TERM_VT220)
			input_dcs_dispatch_decrsts(ictx);
		break;
#ifdef ENABLE_SIXEL
	case INPUT_DCS_SIXEL:
		if (!input_is_graphics_term(ictx->term_level))
			break;
		w = wp->window;
		if (input_split(ictx) != 0)
			return (0);
		p2 = input_get(ictx, 1, 0, 0);
		if (p2 == -1)
			p2 = 0;
		si = sixel_parse(buf, len, p2, w->xpixel, w->ypixel);
		if (si != NULL)
			screen_write_sixelimage(sctx, si, ictx->cell.cell.bg);
		break;
#endif
	}

	return (0);
}

/* Handle a DCS DECRQSS request. */
static void
input_dcs_dispatch_decrqss(struct input_ctx *ictx)
{
	u_char				*buf = ictx->input_buf;
	size_t				 len = ictx->input_len;
	struct window_pane		*wp = ictx->wp;
	struct options			*oo;
	struct screen			*s = ictx->ctx.s;
	struct input_table_entry	*entry;
	char				*seq;
	int				 n;
	const struct input_state	*oldstate;

	/*
	 * Parse the parameter string like it's a CSI sequence, except that
	 * we won't execute the corresponding terminal function and we don't
	 * accept any parameters.
	 */
	oldstate = ictx->state;
	ictx->state = &input_state_decrqss_enter;
	/* Operate on a copy because input_clear() destroys the buffers. */
	seq = xstrndup(buf + 1, len - 1);
	input_clear(ictx);
	input_parse(ictx, seq, len - 1);
	entry = bsearch(ictx, input_csi_table, nitems(input_csi_table),
	    sizeof input_csi_table[0], input_table_compare);
	free(seq);
	ictx->state = oldstate;
	if (ictx->state->enter != NULL)
		ictx->state->enter(ictx);

	if (entry == NULL) {
		log_debug("%s: unknown CSI \"%s%c\"", __func__, ictx->interm_buf,
		    ictx->ch);
		input_reply(ictx, "\033P0$r\033\\");
		return;
	}

	log_debug("%s: '%c' \"%s\"", __func__, ictx->ch, ictx->interm_buf);

	switch (entry->type) {
	case INPUT_CSI_DECSCA:
		/*
		 * Character attribute query: DCS $ q " q ST
		 * Reply: DCS 1 $ r 0 [; <Ps> ...] " q ST
		 */
		n = !!(ictx->cell.cell.attr & GRID_ATTR_PROTECTED) + 1;
		log_debug("%s: DECSCA attributes %d", __func__, n);
		input_reply(ictx, "\033P1$r0;%d\"q\033\\", n);
		break;
	case INPUT_CSI_DECSCL:
		/*
		 * VT conformance level query: DCS $ q " p ST
		 * Reply: DCS 1 $ r <Ps> " p ST
		 */
		switch (ictx->term_level) {
		case TERM_VT100:
		case TERM_VT101:
		case TERM_VT102:
		case TERM_VT125:
			n = 61;
			break;
		case TERM_VT220:
		case TERM_VT241:
			n = 62;
			break;
		}
		log_debug("%s: DECSCL level %d", __func__, n);
		input_reply(ictx, "\033P1$r%d\"p\033\\", n);
		break;
	case INPUT_CSI_DECSCUSR:
		/*
		 * Cursor style query: DCS $ q SP q ST
		 * Reply: DCS 1 $ r <Ps> SP q ST
		 */
		n = s->cstyle;
		if (n > 0 && n <= SCREEN_CURSOR_BAR)
			n = n * 2 - !!(s->mode & MODE_CURSOR_BLINKING);
		else {
			/*
			 * No explicit runtime style: fall back to the
			 * configured cursor-style option (integer Ps 0..6).
			 * Pane options inherit.
			 */
			if (wp != NULL)
				oo = wp->options;
			else
				oo = global_options;
			n = options_get_number(oo, "cursor-style");

			/* Sanity clamp: valid Ps are 0..6 per DECSCUSR. */
			if (n < 0 || n > 6)
				n = 0;
		}
		log_debug("%s: DECSCUSR style = %d", __func__, n);
		input_reply(ictx, "\033P1$r%d q\033\\", n);
		break;
	case INPUT_CSI_SCP_DECSLRM:
		/*
		 * Always DECSLRM in this context.
		 * Left/right margin query: DCS $ q s ST
		 * Reply: DCS 1 $ r <Ps> ; <Ps> s ST
		 */
		log_debug("%s: DECSLRM %d-%d", __func__, s->rleft, s->rright);
		input_reply(ictx, "\033P1$r%d;%ds\033\\",
		    s->rleft + 1, s->rright + 1);
		break;
	case INPUT_CSI_DECSTBM:
		/*
		 * Top/bottom margin query: DCS $ q r ST
		 * Reply: DCS 1 $ r <Ps> ; <Ps> r ST
		 */
		log_debug("%s: DECSTBM %d-%d", __func__, s->rupper, s->rlower);
		input_reply(ictx, "\033P1$r%d;%dr\033\\",
		    s->rupper + 1, s->rlower + 1);
		break;
	case INPUT_CSI_SGR:
		/*
		 * Graphic rendition query: DCS $ q m ST
		 * Reply: DCS 1 $ r 0 [; <Ps> ...] m ST
		 */
		input_reply_decrpss_sgr(ictx);
		break;
	default:
		log_debug("%s: unhandled CSI \"%s%c\"", __func__,
		    ictx->interm_buf, ictx->ch);
		input_reply(ictx, "\033P0$r\033\\");
		break;
	}
}

/* Reply to DECRQSS for SGR with DECRPSS with SGR. */
static void
input_reply_decrpss_sgr(struct input_ctx *ictx)
{
	struct bufferevent	*bev = ictx->event;
	struct grid_cell	*gc = &ictx->cell.cell;
	size_t			 i, n = 0, len;
	int			 mods[10];
	char 			*tmp;
	u_char			 r, g, b;

	if (bev == NULL)
		return;

	if (gc->attr & GRID_ATTR_BRIGHT)
		mods[n++] = 1;
	if (gc->attr & GRID_ATTR_DIM)
		mods[n++] = 2;
	if (gc->attr & GRID_ATTR_ITALICS)
		mods[n++] = 3;
	switch (gc->attr & GRID_ATTR_ALL_UNDERSCORE) {
	case 0:
		break;
	case GRID_ATTR_UNDERSCORE:
		mods[n++] = 4;
		mods[n++] = 1;
		break;
	case GRID_ATTR_UNDERSCORE_2:
		mods[n++] = 21;
		break;
	case GRID_ATTR_UNDERSCORE_3:
		mods[n++] = 4;
		mods[n++] = 3;
		break;
	case GRID_ATTR_UNDERSCORE_4:
		mods[n++] = 4;
		mods[n++] = 4;
		break;
	case GRID_ATTR_UNDERSCORE_5:
		mods[n++] = 4;
		mods[n++] = 5;
		break;
	default:
		fatalx("unhandled underscore type in DECRPSS response");
	}
	if (gc->attr & GRID_ATTR_BLINK)
		mods[n++] = 5;
	if (gc->attr & GRID_ATTR_REVERSE)
		mods[n++] = 7;
	if (gc->attr & GRID_ATTR_HIDDEN)
		mods[n++] = 8;
	if (gc->attr & GRID_ATTR_STRIKETHROUGH)
		mods[n++] = 9;
	if (gc->attr & GRID_ATTR_OVERLINE)
		mods[n++] = 53;
	assertx(n <= 10);
	bufferevent_write(bev, "\033P1$r0", 6);	/* DECRPSS, reset all */
	for (i = 0; i < n; ++i) {
		if (mods[i] == 4) {
			len = xasprintf(&tmp, ";%d:%d", mods[i], mods[i+1]);
			++i;
		} else
			len = xasprintf(&tmp, ";%d", mods[i]);
		log_debug("%s: SGR attr %s", __func__, tmp+1);
		bufferevent_write(bev, tmp, len);
		free(tmp);
	}
	if (!COLOUR_DEFAULT(gc->fg)) {
		if (gc->fg & COLOUR_FLAG_RGB) {
			colour_split_rgb(gc->fg, &r, &g, &b);
			len = xasprintf(&tmp, ";38:2:0:%hhu:%hhu:%hhu",
			    r, g, b);
		} else if (gc->fg & COLOUR_FLAG_256) {
			len = xasprintf(&tmp, ";38:5:%d",
			    gc->fg & ~COLOUR_FLAG_256);
		} else {
			int c = gc->fg;
			if (c <= 8)
				c += 30;
			else
				assertx(c >= 90 && c <= 97);
			len = xasprintf(&tmp, ";%d", c);
		}
		log_debug("%s: SGR fg %s", __func__, tmp+1);
		bufferevent_write(bev, tmp, len);
		free(tmp);
	}
	if (!COLOUR_DEFAULT(gc->bg)) {
		if (gc->bg & COLOUR_FLAG_RGB) {
			colour_split_rgb(gc->bg, &r, &g, &b);
			len = xasprintf(&tmp, ";48:2:0:%hhu:%hhu:%hhu",
			    r, g, b);
		} else if (gc->bg & COLOUR_FLAG_256) {
			len = xasprintf(&tmp, ";48:5:%d",
			    gc->bg & ~COLOUR_FLAG_256);
		} else {
			int c = gc->bg;
			if (c < 8)
				c += 40;
			else {
				assertx(c >= 90 && c <= 97);
				c += 10;
			}
			len = xasprintf(&tmp, ";%d", c);
		}
		log_debug("%s: SGR bg %s", __func__, tmp+1);
		bufferevent_write(bev, tmp, len);
		free(tmp);
	}
	if (!COLOUR_DEFAULT(gc->us)) {
		assertx(gc->us & (COLOUR_FLAG_RGB|COLOUR_FLAG_256));
		if (gc->us & COLOUR_FLAG_RGB) {
			colour_split_rgb(gc->us, &r, &g, &b);
			len = xasprintf(&tmp, ";58:2:0:%hhu:%hhu:%hhu",
			    r, g, b);
		} else if (gc->us & COLOUR_FLAG_256) {
			len = xasprintf(&tmp, ";58:5:%d",
			    gc->us & ~COLOUR_FLAG_256);
		}
		log_debug("%s: SGR us %s", __func__, tmp+1);
		bufferevent_write(bev, tmp, len);
		free(tmp);
	}
	bufferevent_write(bev, "m\033\\", 3);	/* SGR, ST */
}

/* Handle a DCS DECRSPS request. */
static void
input_dcs_dispatch_decrsps(struct input_ctx *ictx)
{
	int			 m;

	m = input_get(ictx, 0, 0, 0);
	switch (m) {
	case -1:
		break;
	case 1:	/* DECCIR */
		input_dcs_dispatch_deccir(ictx);
		break;
	case 2:	/* DECTABSR */
		input_dcs_dispatch_dectabsr(ictx);
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		break;
	}
}

/* Parse a numeric value from a DCS. */
static long long
input_dcs_parse_num(char **ptr, char **out, long long min, long long max,
    const char *desc, const char *sep)
{
	long long	 v;
	const char	*err;

	if (sep) {
		*out = strsep(ptr, sep);
	} else {
		*out = *ptr;
	}
	if (*out == NULL || **out == '\0') {
		log_debug("%s: missing %s", __func__, desc);
		return (-1);
	}
	v = strtonum(*out, min, max, &err);
	if (err != NULL) {
		log_debug("%s: invalid %s \"%s\": %s", __func__, desc, *out,
		    err);
		return (-1);
	}
	return (v);
}

/* Parse graphic-encoded data from a DCS. */
static char
input_dcs_parse_data(char **ptr, char **out, const char *desc, const char *sep)
{
	char		 v;

	if (sep) {
		*out = strsep(ptr, sep);
	} else {
		*out = *ptr;
	}
	if (*out == NULL || **out == '\0') {
		log_debug("%s: missing %s", __func__, desc);
		return (-1);
	}
	v = **out;
	if ((v & 0xe0) != '@') {
		log_debug("%s: invalid %s '%c'", __func__, desc, v);
		return (-1);
	}
	return (v);
}

/* Handle a DCS DECCIR restore request. */
static void
input_dcs_dispatch_deccir(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct screen		*s = sctx->s;
	struct input_cell	*cell = &ictx->cell;
	int			 cx, cy, gl;
	char			 sgr, sca, mode;
	char			*buf = ictx->input_buf, *ptr, *out;
	const char		*g0, *g1, *g2, *g3;

	ptr = buf + 1;
	if ((cy = input_dcs_parse_num(&ptr, &out, 1, screen_size_y(s),
	     "cursor row", ";")) == -1)
		return;
	if ((cx = input_dcs_parse_num(&ptr, &out, 1, screen_size_x(s),
	     "cursor column", ";")) == -1)
		return;
	/* Ignore for now. */
	if (input_dcs_parse_num(&ptr, &out, 1, INT_MAX, "cursor page",
	    ";") == -1)
		return;
	if ((sgr = input_dcs_parse_data(&ptr, &out, "SGR flags", ";")) == -1)
		return;
	if ((sca = input_dcs_parse_data(&ptr, &out, "DECSCA flags", ";")) == -1)
		return;
	if ((mode = input_dcs_parse_data(&ptr, &out, "mode flags", ";")) == -1)
		return;
	if ((gl = input_dcs_parse_num(&ptr, &out, 0, 1, "GL charset #",
	     ";")) == -1)
		return;
	/* Ignore for now. */
	if (input_dcs_parse_num(&ptr, &out, 0, 1, "GR charset #", ";") == -1)
		return;
	/* Ignore for now. */
	if (input_dcs_parse_data(&ptr, &out, "charset flags", ";") == -1)
		return;
	g0 = ptr;
	while (*ptr >= 0x20 && *ptr <= 0x2f)
		ptr++;
	if (*ptr < 0x30 || *ptr >= 0x7f) {
		log_debug("%s: invalid G0 designation \"%.*s\"",
		    __func__, (int)(ptr - g0 + 1), g0);
		return;
	}
	ptr++;
	g1 = ptr;
	while (*ptr >= 0x20 && *ptr <= 0x2f)
		ptr++;
	if (*ptr < 0x30 || *ptr >= 0x7f) {
		log_debug("%s: invalid G1 designation \"%.*s\"",
		    __func__, (int)(ptr - g1 + 1), g1);
		return;
	}
	ptr++;
	g2 = ptr;
	while (*ptr >= 0x20 && *ptr <= 0x2f)
		ptr++;
	if (*ptr < 0x30 || *ptr >= 0x7f) {
		log_debug("%s: invalid G2 designation \"%.*s\"",
		    __func__, (int)(ptr - g2 + 1), g2);
		return;
	}
	ptr++;
	g3 = ptr;
	while (*ptr >= 0x20 && *ptr <= 0x2f)
		ptr++;
	if (*ptr < 0x30 || *ptr >= 0x7f) {
		log_debug("%s: invalid G3 designation \"%.*s\"",
		    __func__, (int)(ptr - g3 + 1), g3);
		return;
	}
	ptr++;

	if (sgr & 0x01)
		cell->cell.attr |= GRID_ATTR_BRIGHT;
	else
		cell->cell.attr &= ~GRID_ATTR_BRIGHT;
	if (sgr & 0x02) {
		if ((cell->cell.attr & GRID_ATTR_ALL_UNDERSCORE) == 0)
			cell->cell.attr |= GRID_ATTR_UNDERSCORE;
	} else
		cell->cell.attr &= ~GRID_ATTR_ALL_UNDERSCORE;
	if (sgr & 0x04)
		cell->cell.attr |= GRID_ATTR_BLINK;
	else
		cell->cell.attr &= ~GRID_ATTR_BLINK;
	if (sgr & 0x08)
		cell->cell.attr |= GRID_ATTR_REVERSE;
	else
		cell->cell.attr &= ~GRID_ATTR_REVERSE;
	if (sca & 0x01)
		cell->cell.attr |= GRID_ATTR_PROTECTED;
	else
		cell->cell.attr &= ~GRID_ATTR_PROTECTED;
	cell->set = gl;
	if (strncmp(g0, "0", 1) == 0)
		cell->g0set = 1;
	else
		cell->g0set = 0;
	if (strncmp(g1, "0", 1) == 0)
		cell->g1set = 1;
	else
		cell->g1set = 0;
	if (mode & 0x01)
		screen_write_mode_set(sctx, MODE_ORIGIN);
	else
		screen_write_mode_clear(sctx, MODE_ORIGIN);
	if (mode & 0x08)
		cx = s->rright + 1;
	screen_write_cursormove(sctx, cx - 1, cy - 1, 1);
}

/* Handle a DCS DECTABSR restore request. */
static void
input_dcs_dispatch_dectabsr(struct input_ctx *ictx)
{
	struct screen		*s = ictx->ctx.s;
	bitstr_t		*tabs;
	int			 st;
	char			*buf = ictx->input_buf, *ptr, *out;
	const char		*err;

	tabs = bit_alloc(screen_size_x(s) - 1);
	ptr = buf + 1;
	while ((out = strsep(&ptr, "/")) != NULL) {
		if (*out == '\0') {
			log_debug("%s: missing tab stop", __func__);
			free(tabs);
			return;
		}
		st = strtonum(out, 0, screen_size_x(s) - 1, &err);
		if (err != NULL) {
			log_debug("%s: invalid tab stop \"%s\": %s", __func__,
			    out, err);
			free(tabs);
			return;
		}
		bit_set(tabs, st - 1);
	}
	free(s->tabs);
	s->tabs = tabs;
}

/* Handle a DCS DECRSTS request. */
static void
input_dcs_dispatch_decrsts(struct input_ctx *ictx)
{
	int			 m;

	m = input_get(ictx, 0, 0, 0);
	switch (m) {
	case -1:
		break;
	case 1:	/* DECTSR */
		log_debug("%s: DECTSR ignored: \"%s\"", __func__,
		    ictx->input_buf + 1);
		break;
	case 2:	/* DECCTR */
		input_dcs_dispatch_decctr(ictx);
		break;
	default:
		log_debug("%s: unknown %d", __func__, m);
		break;
	}
}

/* Handle a DCS DECCTR restore request. */
static void
input_dcs_dispatch_decctr(struct input_ctx *ictx)
{
	char			*buf = ictx->input_buf, *ptr, *out, *param;
	int			*palette;
	int			 i, cs, x, y, z;

	ptr = buf + 1;
	palette = xcalloc(256, sizeof *palette);
	memcpy(palette, ictx->palette->palette, 256 * sizeof *palette);
	while ((out = strsep(&ptr, "/")) != NULL) {
		if (*out == '\0') {
			log_debug("%s: empty colour spec", __func__);
			free(palette);
			return;
		}
		if ((i = input_dcs_parse_num(&out, &param, 0, 255,
		     "palette index", ";")) == -1) {
			free(palette);
			return;
		}
		if ((cs = input_dcs_parse_num(&out, &param, 1, 2,
		     "colour space", ";")) == -1) {
			free(palette);
			return;
		}
		if ((x = input_dcs_parse_num(&out, &param, 0,
		     cs == 1 ? 360 : 100, "colour x", ";")) == -1) {
			free(palette);
			return;
		}
		if ((y = input_dcs_parse_num(&out, &param, 0, 100, "colour y",
		     ";")) == -1) {
			free(palette);
			return;
		}
		if ((z = input_dcs_parse_num(&out, &param, 0, 100, "colour z",
		     NULL)) == -1) {
			free(palette);
			return;
		}

		switch (cs) {
		case 1:
			palette[i] = colour_join_hls(x, y, z);
			break;
		case 2:
			x = x * 255 / 100;
			y = y * 255 / 100;
			z = z * 255 / 100;
			palette[i] = colour_join_rgb(x, y, z);
			break;
		}
	}
	free(ictx->palette->palette);
	ictx->palette->palette = palette;
}

/* OSC string started. */
static void
input_enter_osc(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* OSC terminator (ST) received. */
static void
input_exit_osc(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	u_char			*p = ictx->input_buf;
	u_int			 option;

	if (ictx->flags & INPUT_DISCARD)
		return;
	if (ictx->input_len < 1 || *p < '0' || *p > '9')
		return;

	log_debug("%s: \"%s\" (end %s)", __func__, p,
	    ictx->input_end == INPUT_END_ST ? "ST" : "BEL");

	option = 0;
	while (*p >= '0' && *p <= '9')
		option = option * 10 + *p++ - '0';
	if (*p != ';' && *p != '\0')
		return;
	if (*p == ';')
		p++;

	switch (option) {
	case 0:
	case 2:
		if (wp != NULL &&
		    options_get_number(wp->options, "allow-set-title") &&
		    screen_set_title(sctx->s, p)) {
			notify_pane("pane-title-changed", wp);
			server_redraw_window_borders(wp->window);
			server_status_window(wp->window);
		}
		break;
	case 4:
		input_osc_4(ictx, p);
		break;
	case 7:
		if (utf8_isvalid(p)) {
			screen_set_path(sctx->s, p);
			if (wp != NULL) {
				server_redraw_window_borders(wp->window);
				server_status_window(wp->window);
			}
		}
		break;
	case 8:
		input_osc_8(ictx, p);
		break;
	case 10:
		input_osc_10(ictx, p);
		break;
	case 11:
		input_osc_11(ictx, p);
		break;
	case 12:
		input_osc_12(ictx, p);
		break;
	case 52:
		input_osc_52(ictx, p);
		break;
	case 104:
		input_osc_104(ictx, p);
		break;
	case 110:
		input_osc_110(ictx, p);
		break;
	case 111:
		input_osc_111(ictx, p);
		break;
	case 112:
		input_osc_112(ictx, p);
		break;
	case 133:
		input_osc_133(ictx, p);
		break;
	default:
		log_debug("%s: unknown '%u'", __func__, option);
		break;
	}
}

/* APC string started. */
static void
input_enter_apc(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* APC terminator (ST) received. */
static void
input_exit_apc(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;

	if (ictx->flags & INPUT_DISCARD)
		return;
	log_debug("%s: \"%s\"", __func__, ictx->input_buf);

	if (screen_set_title(sctx->s, ictx->input_buf) && wp != NULL) {
		notify_pane("pane-title-changed", wp);
		server_redraw_window_borders(wp->window);
		server_status_window(wp->window);
	}
}

/* Rename string started. */
static void
input_enter_rename(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* Rename terminator (ST) received. */
static void
input_exit_rename(struct input_ctx *ictx)
{
	struct window_pane	*wp = ictx->wp;
	struct window		*w;
	struct options_entry	*o;

	if (wp == NULL)
		return;
	if (ictx->flags & INPUT_DISCARD)
		return;
	if (!options_get_number(ictx->wp->options, "allow-rename"))
		return;
	log_debug("%s: \"%s\"", __func__, ictx->input_buf);

	if (!utf8_isvalid(ictx->input_buf))
		return;
	w = wp->window;

	if (ictx->input_len == 0) {
		o = options_get_only(w->options, "automatic-rename");
		if (o != NULL)
			options_remove_or_default(o, -1, NULL);
		if (!options_get_number(w->options, "automatic-rename"))
			window_set_name(w, "");
	} else {
		options_set_number(w->options, "automatic-rename", 0);
		window_set_name(w, ictx->input_buf);
	}
	server_redraw_window_borders(w);
	server_status_window(w);
}

/* Open UTF-8 character. */
static int
input_top_bit_set(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct utf8_data	*ud = &ictx->utf8data;

	ictx->flags &= ~INPUT_LAST;

	if (!ictx->utf8started) {
		ictx->utf8started = 1;
		if (utf8_open(ud, ictx->ch) != UTF8_MORE)
			input_stop_utf8(ictx);
		return (0);
	}

	switch (utf8_append(ud, ictx->ch)) {
	case UTF8_MORE:
		return (0);
	case UTF8_ERROR:
		input_stop_utf8(ictx);
		return (0);
	case UTF8_DONE:
		break;
	}
	ictx->utf8started = 0;

	log_debug("%s %hhu '%*s' (width %hhu)", __func__, ud->size,
	    (int)ud->size, ud->data, ud->width);

	utf8_copy(&ictx->cell.cell.data, ud);
	screen_write_collect_add(sctx, &ictx->cell.cell);

	utf8_copy(&ictx->last, &ictx->cell.cell.data);
	ictx->flags |= INPUT_LAST;

	return (0);
}

/* Reply to a colour request. */
static void
input_osc_colour_reply(struct input_ctx *ictx, u_int n, int idx, int c)
{
	u_char		 r, g, b;
	const char	*end;

	if (c != -1)
		c = colour_force_rgb(c);
	if (c == -1)
	    return;
	colour_split_rgb(c, &r, &g, &b);

	if (ictx->input_end == INPUT_END_BEL)
		end = "\007";
	else
		end = "\033\\";

	if (n == 4) {
		input_reply(ictx,
		    "\033]%u;%d;rgb:%02hhx%02hhx/%02hhx%02hhx/%02hhx%02hhx%s",
		    n, idx, r, r, g, g, b, b, end);
	} else {
		input_reply(ictx,
		    "\033]%u;rgb:%02hhx%02hhx/%02hhx%02hhx/%02hhx%02hhx%s",
		    n, r, r, g, g, b, b, end);
	}
}

/* Handle the OSC 4 sequence for setting (multiple) palette entries. */
static void
input_osc_4(struct input_ctx *ictx, const char *p)
{
	char			*copy, *s, *next = NULL;
	long			 idx;
	int			 c, bad = 0, redraw = 0;
	struct colour_palette	*palette = ictx->palette;

	copy = s = xstrdup(p);
	while (s != NULL && *s != '\0') {
		idx = strtol(s, &next, 10);
		if (*next++ != ';') {
			bad = 1;
			break;
		}
		if (idx < 0 || idx >= 256) {
			bad = 1;
			break;
		}

		s = strsep(&next, ";");
		if (strcmp(s, "?") == 0) {
			c = colour_palette_get(palette, idx|COLOUR_FLAG_256);
			if (c != -1)
				input_osc_colour_reply(ictx, 4, idx, c);
			s = next;
			continue;
		}
		if ((c = colour_parseX11(s)) == -1) {
			s = next;
			continue;
		}
		if (colour_palette_set(palette, idx, c))
			redraw = 1;
		s = next;
	}
	if (bad)
		log_debug("bad OSC 4: %s", p);
	if (redraw)
		screen_write_fullredraw(&ictx->ctx);
	free(copy);
}

/* Handle the OSC 8 sequence for embedding hyperlinks. */
static void
input_osc_8(struct input_ctx *ictx, const char *p)
{
	struct hyperlinks	*hl = ictx->ctx.s->hyperlinks;
	struct grid_cell	*gc = &ictx->cell.cell;
	const char		*start, *end, *uri;
	char			*id = NULL;

	for (start = p; (end = strpbrk(start, ":;")) != NULL; start = end + 1) {
		if (end - start >= 4 && strncmp(start, "id=", 3) == 0) {
			if (id != NULL)
				goto bad;
			id = xstrndup(start + 3, end - start - 3);
		}

		/* The first ; is the end of parameters and start of the URI. */
		if (*end == ';')
			break;
	}
	if (end == NULL || *end != ';')
		goto bad;
	uri = end + 1;
	if (*uri == '\0') {
		gc->link = 0;
		free(id);
		return;
	}
	gc->link = hyperlinks_put(hl, uri, id);
	if (id == NULL)
		log_debug("hyperlink (anonymous) %s = %u", uri, gc->link);
	else
		log_debug("hyperlink (id=%s) %s = %u", id, uri, gc->link);
	free(id);
	return;

bad:
	log_debug("bad OSC 8 %s", p);
	free(id);
}


/* Handle the OSC 10 sequence for setting and querying foreground colour. */
static void
input_osc_10(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	struct grid_cell	 defaults;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp == NULL)
			return;
		c = window_pane_get_fg_control_client(wp);
		if (c == -1) {
			tty_default_colours(&defaults, wp);
			if (COLOUR_DEFAULT(defaults.fg))
				c = window_pane_get_fg(wp);
			else
				c = defaults.fg;
		}
		input_osc_colour_reply(ictx, 10, 0, c);
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 10: %s", p);
		return;
	}
	if (ictx->palette != NULL) {
		ictx->palette->fg = c;
		if (wp != NULL)
			wp->flags |= PANE_STYLECHANGED;
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 110 sequence for resetting foreground colour. */
static void
input_osc_110(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;

	if (*p != '\0')
		return;
	if (ictx->palette != NULL) {
		ictx->palette->fg = 8;
		if (wp != NULL)
			wp->flags |= PANE_STYLECHANGED;
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 11 sequence for setting and querying background colour. */
static void
input_osc_11(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp == NULL)
			return;
		c = window_pane_get_bg(wp);
		input_osc_colour_reply(ictx, 11, 0, c);
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 11: %s", p);
		return;
	}
	if (ictx->palette != NULL) {
		ictx->palette->bg = c;
		if (wp != NULL)
			wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 111 sequence for resetting background colour. */
static void
input_osc_111(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;

	if (*p != '\0')
		return;
	if (ictx->palette != NULL) {
		ictx->palette->bg = 8;
		if (wp != NULL)
			wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 12 sequence for setting and querying cursor colour. */
static void
input_osc_12(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp != NULL) {
			c = ictx->ctx.s->ccolour;
			if (c == -1)
				c = ictx->ctx.s->default_ccolour;
			input_osc_colour_reply(ictx, 12, 0, c);
		}
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 12: %s", p);
		return;
	}
	screen_set_cursor_colour(ictx->ctx.s, c);
}

/* Handle the OSC 112 sequence for resetting cursor colour. */
static void
input_osc_112(struct input_ctx *ictx, const char *p)
{
	if (*p == '\0') /* no arguments allowed */
		screen_set_cursor_colour(ictx->ctx.s, -1);
}

/* Handle the OSC 133 sequence. */
static void
input_osc_133(struct input_ctx *ictx, const char *p)
{
	struct grid		*gd = ictx->ctx.s->grid;
	u_int			 line = ictx->ctx.s->cy + gd->hsize;
	struct grid_line	*gl;

	if (line > gd->hsize + gd->sy - 1)
		return;
	gl = grid_get_line(gd, line);

	switch (*p) {
	case 'A':
		gl->flags |= GRID_LINE_START_PROMPT;
		break;
	case 'C':
		gl->flags |= GRID_LINE_START_OUTPUT;
		break;
	}
}

/* Handle the OSC 52 sequence for setting the clipboard. */
static void
input_osc_52(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	char			*end;
	const char		*buf = NULL;
	size_t			 len = 0;
	u_char			*out;
	int			 outlen, state;
	struct screen_write_ctx	 ctx;
	struct paste_buffer	*pb;
	const char*		 allow = "cpqs01234567";
	char			 flags[sizeof "cpqs01234567"] = "";
	u_int			 i, j = 0;

	if (wp == NULL)
		return;
	state = options_get_number(global_options, "set-clipboard");
	if (state != 2)
		return;

	if ((end = strchr(p, ';')) == NULL)
		return;
	end++;
	if (*end == '\0')
		return;
	log_debug("%s: %s", __func__, end);

	for (i = 0; p + i != end; i++) {
		if (strchr(allow, p[i]) != NULL && strchr(flags, p[i]) == NULL)
			flags[j++] = p[i];
	}
	log_debug("%s: %.*s %s", __func__, (int)(end - p - 1), p, flags);

	if (strcmp(end, "?") == 0) {
		if ((pb = paste_get_top(NULL)) != NULL)
			buf = paste_buffer_data(pb, &len);
		if (ictx->input_end == INPUT_END_BEL)
			input_reply_clipboard(ictx->event, buf, len, "\007");
		else
			input_reply_clipboard(ictx->event, buf, len, "\033\\");
		return;
	}

	len = (strlen(end) / 4) * 3;
	if (len == 0)
		return;

	out = xmalloc(len);
	if ((outlen = b64_pton(end, out, len)) == -1) {
		free(out);
		return;
	}

	screen_write_start_pane(&ctx, wp, NULL);
	screen_write_setselection(&ctx, flags, out, outlen);
	screen_write_stop(&ctx);
	notify_pane("pane-set-clipboard", wp);

	paste_add(NULL, out, outlen);
}

/* Handle the OSC 104 sequence for unsetting (multiple) palette entries. */
static void
input_osc_104(struct input_ctx *ictx, const char *p)
{
	char	*copy, *s;
	long	 idx;
	int	 bad = 0, redraw = 0;

	if (*p == '\0') {
		colour_palette_clear(ictx->palette);
		screen_write_fullredraw(&ictx->ctx);
		return;
	}

	copy = s = xstrdup(p);
	while (*s != '\0') {
		idx = strtol(s, &s, 10);
		if (*s != '\0' && *s != ';') {
			bad = 1;
			break;
		}
		if (idx < 0 || idx >= 256) {
			bad = 1;
			break;
		}
		if (colour_palette_set(ictx->palette, idx, -1))
			redraw = 1;
		if (*s == ';')
			s++;
	}
	if (bad)
		log_debug("bad OSC 104: %s", p);
	if (redraw)
		screen_write_fullredraw(&ictx->ctx);
	free(copy);
}

void
input_reply_clipboard(struct bufferevent *bev, const char *buf, size_t len,
    const char *end)
{
	char	*out = NULL;
	int	 outlen = 0;

	if (buf != NULL && len != 0) {
		if (len >= ((size_t)INT_MAX * 3 / 4) - 1)
			return;
		outlen = 4 * ((len + 2) / 3) + 1;
		out = xmalloc(outlen);
		if ((outlen = b64_ntop(buf, len, out, outlen)) == -1) {
			free(out);
			return;
		}
	}

	bufferevent_write(bev, "\033]52;;", 6);
	if (outlen != 0)
		bufferevent_write(bev, out, outlen);
	bufferevent_write(bev, end, strlen(end));
	free(out);
}

/* Set input buffer size. */
void
input_set_buffer_size(size_t buffer_size)
{
	log_debug("%s: %lu -> %lu", __func__, input_buffer_size, buffer_size);
	input_buffer_size = buffer_size;
}

static void
input_report_current_theme(struct input_ctx *ictx)
{
	switch (window_pane_get_theme(ictx->wp)) {
		case THEME_DARK:
			input_reply(ictx, "\033[?997;1n");
			break;
		case THEME_LIGHT:
			input_reply(ictx, "\033[?997;2n");
			break;
		case THEME_UNKNOWN:
			break;
	}
}
