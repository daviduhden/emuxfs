# CI orchestration regression fixtures; run from the repository root.
# Substitutes only host prerequisites and installed paths in a temporary copy.
import os, subprocess, tempfile
from pathlib import Path
source=Path('scripts/ci-openbsd.pl').read_text()
with tempfile.TemporaryDirectory(prefix='emuxfs-perl-ci-test-') as tmp:
 root=Path(tmp); (root/'tests/integration').mkdir(parents=True);(root/'tests/unit').mkdir();(root/'bin').mkdir()
 log=root/'commands'
 for name in ['make','mandoc','man','ldd']:
  p=root/'bin'/name;p.write_text('#!/bin/sh\nprintf "%s\\n" "'+name+' $*" >> "$COMMAND_LOG"\n'+('echo "STYLE: test fixture"\n' if name=='mandoc' else '')+'exit 0\n');p.chmod(0o755)
 for name in ['emuxfs','tests/unit/test_core']:
  p=root/name;p.write_text('#!/bin/sh\nexit 0\n');p.chmod(0o755)
 for name in ['integration','parallel','crash']:(root/f'tests/integration/{name}.pl').write_text('print "fixture passed\\n";\n')
 (root/'manual').touch()
 # Only environmental preconditions/installed paths differ; run exact CI logic.
 script=source.replace("'/dev/fuse0'","'/dev/null'").replace("unless $> == 0",'unless 1').replace("'/usr/local/sbin/emuxfs'",repr(str(root/'emuxfs'))).replace("'/usr/local/man/man1/emuxfs.1'",repr(str(root/'manual')))
 p=root/'ci.pl';p.write_text(script)
 env=os.environ.copy();env.update(GITHUB_WORKSPACE=tmp,COMMAND_LOG=str(log),PATH=str(root/'bin')+':'+env['PATH'])
 r=subprocess.run(['perl',str(p)],env=env,capture_output=True,text=True);assert r.returncode==0,(r.stdout,r.stderr)
 assert 'All CI steps passed.' in r.stdout
 commands=log.read_text();assert 'make EMUXFS_DS_MALLOC=1 unittest' in commands
 (root/'bin/mandoc').write_text('#!/bin/sh\necho "WARNING: fixture"\nexit 1\n')
 r=subprocess.run(['perl',str(p)],env=env,capture_output=True,text=True);assert r.returncode==1 and 'mandoc reported' in r.stderr
 (root/'bin/mandoc').write_text('#!/bin/sh\nexit 0\n')
 (root/'tests/integration/integration.pl').write_text('print "FAIL: fixture\\n";\n')
 r=subprocess.run(['perl',str(p)],env=env,capture_output=True,text=True);assert r.returncode==1 and 'integration reported' in r.stderr
 print('CI orchestration fixtures: successful sequence, STYLE allowance, mandoc WARNING and integration FAIL rejection PASS')
