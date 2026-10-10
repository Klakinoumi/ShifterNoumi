#include "debug_trace.h"

#include "gear_position.h"
#include "hid_report.h"

#include <string.h>

/* Buffer d'une ligne CSV. Dimensionne pour la ligne CAL, la plus longue :
 * "CAL," + t_ms (10) + 54 x ",4095" = ~285 caracteres ; une ligne de pas
 * lent ne depasse pas ~180 caracteres. */
#define DEBUG_TRACE_LINE_MAX 320

#ifdef DEBUG_TRACE_HOST_TEST
/* Hors cible (tests App/test) : pas de CDC reel, on capture juste la
 * derniere ligne produite pour verifier le formatage. */
char debug_trace_host_buffer[DEBUG_TRACE_LINE_MAX];
static void trace_send_line(const char *line, uint16_t len)
{
    memcpy(debug_trace_host_buffer, line, len);
    debug_trace_host_buffer[len] = '\0';
}
static int CDC_IF_TxReady(void)
{
    return 1;
}
#else
#include "usbd_cdc_if.h"
static void trace_send_line(const char *line, uint16_t len)
{
    CDC_IF_Transmit((uint8_t *)line, len);
}
#endif

typedef struct {
    char buf[DEBUG_TRACE_LINE_MAX];
    uint16_t len;
} line_writer_t;

/* Buffer de ligne STATIQUE (pas une variable locale de debug_trace_step) :
 * USBD_COMPOSITE_CDC_SetTxBuffer ne fait que stocker le pointeur passe
 * (pas de copie), et USBD_LL_Transmit demarre une transmission USB
 * asynchrone que l'IRQ continue de lire paquet par paquet apres le retour
 * de cette fonction. Un buffer sur la pile serait donc ecrase (par l'appel
 * suivant/HAL_Delay) avant la fin reelle de la transmission — c'etait le
 * bug d'origine (corruption observee dans test1_cdc.csv : des octets d'une
 * autre variable/frame se melangeaient dans le flux, ex. HID_EPIN_SIZE=0x10
 * qui fuitait dans t_ms). */
static line_writer_t g_line;

static void lw_reset(line_writer_t *w)
{
    w->len = 0;
}

static void lw_char(line_writer_t *w, char c)
{
    if (w->len + 1 < DEBUG_TRACE_LINE_MAX) {
        w->buf[w->len++] = c;
    }
}

static void lw_str(line_writer_t *w, const char *s)
{
    while (*s) {
        lw_char(w, *s++);
    }
}

/* Entier non signe -> decimal, sans division flottante ni snprintf (cout
 * CPU minimal, appele plusieurs fois par pas lent 100ms). */
static void lw_u32(line_writer_t *w, uint32_t v)
{
    char digits[10];
    int i = 0;
    if (v == 0) {
        lw_char(w, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(digits)) {
        digits[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) {
        lw_char(w, digits[--i]);
    }
}

static void lw_i32(line_writer_t *w, int32_t v)
{
    if (v < 0) {
        lw_char(w, '-');
        lw_u32(w, (uint32_t)(-v));
    } else {
        lw_u32(w, (uint32_t)v);
    }
}

static const char *tracker_state_name(tracker_state_t s)
{
    switch (s) {
    case TRACKER_IDLE:
        return "IDLE";
    case TRACKER_PENDING:
        return "PENDING";
    case TRACKER_RISING:
        return "RISING";
    case TRACKER_HELD:
        return "HELD";
    default:
        return "?";
    }
}

static const char *movement_kind_name(movement_kind_t k)
{
    switch (k) {
    case MOVEMENT_KIND_NONE:
        return "";
    case MOVEMENT_KIND_SEQ:
        return "SEQ";
    case MOVEMENT_KIND_HELD:
        return "HELD";
    case MOVEMENT_KIND_HELD_CORRECTED:
        return "HELD_CORRECTED";
    default:
        return "?";
    }
}

void debug_trace_init(void)
{
    static const char header[] =
        "t_ms,u2,u3,u4,u5,u6,u7,collar,tracker_state,mode,report_debug,"
        "emit_pos,emit_kind,switch_target,"
        "gear6_active,gear6_blips,seqdown_window,seqdown_count,seqdown_blips,"
        "calib_active,calib_step,calib_target,fast_pos,buttons\r\n";
    trace_send_line(header, (uint16_t)(sizeof(header) - 1U));
}

/* Derniere calibration envoyee (ligne CAL) et quand. */
static bool g_cal_sent;
static uint32_t g_cal_sent_t;
static hall_sample_t g_cal_sent_positions[GEAR_POSITION_COUNT];

void debug_trace_calibration_poll(uint32_t t_ms, const gear_calibration_t *calibration)
{
    bool changed = !g_cal_sent ||
                   memcmp(g_cal_sent_positions, calibration->positions, sizeof(g_cal_sent_positions)) != 0;
    if (!changed && (t_ms - g_cal_sent_t) < DEBUG_TRACE_CAL_PERIOD_MS) {
        return;
    }
    if (!CDC_IF_TxReady()) {
        return; /* reessaye au tour suivant */
    }

    line_writer_t *w = &g_line;
    lw_reset(w);
    lw_str(w, "CAL,");
    lw_u32(w, t_ms);
    for (int p = 0; p < GEAR_POSITION_COUNT; p++) {
        for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
            lw_char(w, ',');
            lw_u32(w, calibration->positions[p].v[i]);
        }
    }
    lw_str(w, "\r\n");
    trace_send_line(w->buf, w->len);

    memcpy(g_cal_sent_positions, calibration->positions, sizeof(g_cal_sent_positions));
    g_cal_sent_t = t_ms;
    g_cal_sent = true;
}

void debug_trace_step(uint32_t t_ms, const hall_sample_t *hall, int collar, const mode_state_machine_t *msm,
                       const uint8_t *report, gear_position_t fast_pos)
{
    /* Ne pas ecraser g_line si la transmission precedente n'est pas
     * terminee (buffer encore lu par l'IRQ USB) — on saute simplement ce
     * pas, cohere avec le choix "non bloquant, ligne perdue occasionnelle"
     * documente dans debug_trace.h. */
    if (!CDC_IF_TxReady()) {
        return;
    }

    line_writer_t *w = &g_line;
    lw_reset(w);

    lw_u32(w, t_ms);
    for (int i = 0; i < HALL_SENSOR_COUNT; i++) {
        lw_char(w, ',');
        lw_u32(w, hall->v[i]);
    }
    lw_char(w, ',');
    lw_u32(w, (uint32_t)collar);

    lw_char(w, ',');
    lw_str(w, tracker_state_name(msm->tracker.state));
    lw_char(w, ',');
    lw_str(w, msm->mode_courant == GEAR_MODE_SEQ ? "SEQ" : "H");
    lw_char(w, ',');
    lw_u32(w, report[HID_DEBUG_BYTE_INDEX]);

    lw_char(w, ',');
    if (msm->last_emitted.pos != GEAR_NONE) {
        lw_str(w, gear_position_name(msm->last_emitted.pos));
    }
    lw_char(w, ',');
    lw_str(w, movement_kind_name(msm->last_emitted.kind));

    lw_char(w, ',');
    if (msm->last_switch_applied) {
        lw_str(w, msm->last_switch_target == GESTURE_TARGET_H ? "H" : "SEQ");
    }

    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->gesture.gear6_active);
    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->gesture.gear6_blip_count);
    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->gesture.seqdown_window_active);
    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->gesture.seqdown_count);
    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->gesture.seqdown_blip_count);

    lw_char(w, ',');
    lw_u32(w, (uint32_t)msm->calib_mode.active);
    lw_char(w, ',');
    if (msm->calib_mode.active) {
        lw_i32(w, msm->calib_mode.step_index);
    }
    lw_char(w, ',');
    if (msm->calib_mode.active) {
        lw_str(w, gear_position_name(calibration_mode_current_target(&msm->calib_mode)));
    }

    lw_char(w, ',');
    if (fast_pos != GEAR_NONE) {
        lw_str(w, gear_position_name(fast_pos));
    }
    lw_char(w, ',');
    lw_u32(w, (uint32_t)(report[0] | (report[1] << 8)));

    lw_str(w, "\r\n");

    trace_send_line(w->buf, w->len);
}
