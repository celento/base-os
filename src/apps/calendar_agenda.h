#ifndef CALENDAR_AGENDA_H
#define CALENDAR_AGENDA_H

#define CAL_AGENDA_MAX 128
#define CAL_AGENDA_TITLE_MAX 95
#define CAL_AGENDA_YEAR_MIN 1900
#define CAL_AGENDA_YEAR_MAX 9999
#define CAL_AGENDA_FILE_MAX (32 + CAL_AGENDA_MAX * 108 + 113)

typedef struct {
    unsigned id;
    int year, month, day;
    int minute; /* -1 = all-day; otherwise minutes after midnight, 0..1439. */
    char title[CAL_AGENDA_TITLE_MAX + 1];
} CalAppointment;

typedef struct {
    unsigned edit_id; /* 0 = new appointment; otherwise existing stable ID. */
    int active, field; /* field: 0 = date, 1 = time, 2 = title. */
    char date[11], time[6], title[CAL_AGENDA_TITLE_MAX + 1];
} CalAgendaDraft;

/* Reopen never discards pending changes or an active editor draft. */
void cal_agenda_load(void);
/* After storage progress, before autosync, even while Calendar is closed. */
int cal_agenda_tick(void);
const char *cal_agenda_status(void);
/* Explicit blocking retry: 0 durable, FS_ERR_BUSY busy, -1 failure. */
int cal_agenda_retry_save(void);
/* Drains old snapshot, stages accepted appointments AND unfinished draft.
 * Caller must fs_sync() after all shutdown staging and check its result. */
int cal_agenda_prepare_shutdown(void);
int cal_agenda_count(void);
/* Sorted date, all-day first, time, then ID. Pointers valid until mutation. */
const CalAppointment *cal_agenda_get(int index);
const CalAppointment *cal_agenda_find(unsigned id);
/* Accepted in private memory even when disk is unavailable; inspect status.
 * CRUD queues a write for tick, never performs filesystem mutation or I/O. */
unsigned cal_agenda_add(int year, int month, int day, int minute, const char *title);
int cal_agenda_update(unsigned id, int year, int month, int day, int minute, const char *title);
/* Rejects deleting the appointment in an active draft; cancel it explicitly. */
int cal_agenda_delete(unsigned id);
const CalAgendaDraft *cal_agenda_draft(void);
/* Partial/invalid date and time text survives recovery; only bounded printable
 * ASCII is accepted. No pointer into model may be modified by the caller. */
int cal_agenda_set_draft(const CalAgendaDraft *draft);
int cal_agenda_clear_draft(void);
/* Strictly validates, accepts appointment, and clears draft atomically.
 * Returns ID or 0 (invalid/full/exhausted); failed acceptance keeps all fields. */
unsigned cal_agenda_commit_draft(void);

int cal_agenda_days_in_month(int year, int month);
int cal_agenda_valid_date(int year, int month, int day);
int cal_agenda_parse_date(const char *text, int *year, int *month, int *day);
int cal_agenda_parse_time(const char *text, int *minute);
/* dir must be -1 or +1. Month stepping clamps day; no range-end wrap. */
int cal_agenda_step_day(int *year, int *month, int *day, int dir);
int cal_agenda_step_month(int *year, int *month, int *day, int dir);

#endif
