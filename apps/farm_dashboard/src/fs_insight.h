#ifndef FS_INSIGHT_H
#define FS_INSIGHT_H

/* derived-channel read() callbacks: return 0 and fill *raw, or -1 for "no value yet" */
int ins_read_gap(float *raw);     /* local soil  minus regional surface wetness */
int ins_read_tgap(float *raw);    /* local air T minus regional T2M             */
int ins_read_corr(float *raw);    /* Pearson r, local soil vs regional surface  */
int ins_read_irr(float *raw);     /* irrigation need 0..100                     */

int         ins_pair_count(void);
int         ins_verdict(char *reason, int n);   /* returns level 0..3 */
const char *ins_level_text(int level);
const char *ins_source(void);
void        ins_set_source(const char *s);

#endif
