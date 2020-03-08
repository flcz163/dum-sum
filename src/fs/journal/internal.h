int log_start_commit(struct journal *journal, trans_id_t tid);
int __log_start_commit(struct journal *journal, trans_id_t tid);

int journal_checkpoint(struct journal *journal);
void __journal_wait_space(struct journal *journal);
int __log_free_space(struct journal *);