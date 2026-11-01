"""Actual CLI batch state machine under host ASan/UBSan; adapters mocked."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    out=pathlib.Path(tempfile.mkdtemp(prefix='pkg-batch-',dir=ROOT/'.codex-remote-attachments'))
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-DTOOL_HOST_TEST','-DFORTRESS_PKG_FETCH=1','-I.','-Isrc/include','-Isrc/fs','-Isrc/kernel','-Iuser/tools']
    subprocess.run(flags+['tests/pkg_batch_cli_host.c','user/pkg/system_cli.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/common.c','-o',str(out/'batch')],cwd=ROOT,check=True)
    subprocess.run([str(out/'batch')],check=True)
    print('Evidence:',out)
if __name__=='__main__':main()
