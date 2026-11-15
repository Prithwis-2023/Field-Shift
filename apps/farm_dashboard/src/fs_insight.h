#ifndef FS_INSIGHT_H
#define FS_INSIGHT_H

/* change when the real regional feed is connected */
const char *ins_source(void);
void ins_set_source(const char *s);

/* derived-channel read() callbacks: return 0 and fill *raw, or -1 for "no value yet" */
int ins_read_gap(float *raw);
int ins_read_corr(float *raw);
int ins_read_irr(float *raw);

int         ins_pair_count(void);
int         ins_verdict(char *reason, int n);   /* returns level 0..3 */
const char *ins_level_text(int level);

#endif
