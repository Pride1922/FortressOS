"""Copied-source CLI bridge to the accepted private engine, default entry unchanged."""
from pkg_retirement_idle_overlay import apply_idle
from test_pkg_service_integration import function

def apply_cli(work,replace,generated):
 apply_idle(work,replace,generated)
 replace('tests/pkg_retirement_idle.inc','int main(int argc,char **argv){','int qt_previous_main(int argc,char **argv){')
 replace('tests/pkg_account_durable.inc','ad_previews++;int r=ad_validate(e);if(!r)ng_preview(e);',
         'ad_previews++;int r=ad_validate(e);if(!r)ng_preview(e);\n    if(!r&&qt_observe){qt_capture();qt_observed=true;return -VFS_EAGAIN;}')
 p=work/'tests/pkg_mount_admission.inc';text=p.read_text();text=text.replace(function(text,'long tool_syscall('),'');p.write_text(text)
 text=generated.read_text();needle='static unsigned xi_stage;'
 assert text.count(needle)==1
 text=text.replace(needle,'#include "user/pkg/transaction_cli.h"\nstatic bool qt_observe,qt_observed;\nstatic void qt_capture(void);\n'+needle)
 generated.write_text(text+'\n#include "pkg_cli_transaction.inc"\n')
