import base64
import json
import os
import subprocess
import sys

def main():
    root = os.path.abspath(sys.argv[1])
    os.chdir(root)

    ccbin = os.path.abspath(sys.argv[2].replace('/', os.path.sep))
    js = base64.b64decode(bytes(sys.argv[3], encoding='utf-8'))
    cmds = json.loads(js)

    if cmds['txt']:
        res = subprocess.run([ccbin] + cmds['run'].split(' '), cwd=root, capture_output=True, universal_newlines=True)
        out = res.stdout
        if out != cmds['txt']:
            print('got output:\n')
            print(out)
            print('but expected:\n')
            print(cmds['txt'])
            return 1
    else:
        res = subprocess.run([ccbin] + cmds['run'].split(' '), cwd=root)

    if res.returncode != cmds['ret']:
        print('got return code %d, but expected %d' % (res.returncode, cmds['ret']))
        return 2


if __name__ == '__main__':
    sys.exit(main())
