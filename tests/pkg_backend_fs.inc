/* Actual extracted storage through the private value contract. Host only:
 * registry is actual, owner birth/transport are supplied by this harness. */
#include "pkg_backend_candidate.h"
#include "process_table.h"
static pkg_transaction_backend pb_storage;
static pw_key pb_owner;
static creds_t pb_credentials;
static void pb_cli_value(const pb_result *r,pkg_transaction_status *s){
 *s=(pkg_transaction_status){.phase=r->phase,.sequence=r->sequence,.account_sequence=r->account_sequence,.credential_generation=r->credential_generation};
 for(unsigned i=0;i<2;i++){s->remaining[i]=r->remaining[i];s->charged[i]=r->charged[i];}
}
static int qt_status(void *context,pkg_transaction_status *out){
 pb_storage=(pkg_transaction_backend){context,pb_engine_status,pb_engine_recover};
 pb_request q={.size=32,.version=1,.operation=PB_STATUS};pb_result result;
 int r=pb_control(&q,pb_owner,true,&pb_storage,&result);if(!r)pb_cli_value(&result,out);return r;
}
static int qt_recover(void *context,pkg_transaction_status *out){
 assert(!process_record_begin(777,0,false,0,0));creds_init_root(&pb_credentials);
 assert(process_record_bind_creds(777,&pb_credentials));process_record_commit(777);assert(pw_register(777,&pb_owner));
 pb_storage=(pkg_transaction_backend){context,pb_engine_status,pb_engine_recover};
 pb_request q={.size=32,.version=1,.operation=PB_ACQUIRE};pb_result result;
 unsigned calls=qt_status_calls;int r=pb_control(&q,pb_owner,true,&pb_storage,&result);qt_status_calls=calls;
 if(!r){
  q.operation=PB_RECOVER;q.session=result.session;r=pb_control(&q,pb_owner,true,&pb_storage,&result);
  if(!r)pb_cli_value(&result,out);
  q.operation=PB_RELEASE;assert(!pb_control(&q,pb_owner,true,&pb_storage,&result));
 }else{qt_recover_calls++;qt_backend_error=r;}
 assert(pw_detach(pb_owner));(void)process_record_exit(777,0);process_record_forget(777);return r;
}
