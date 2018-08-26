import r2pipe
import json
import sys
import os.path
from os import listdir

def import_ggfile (input_path):
    r = r2pipe.open("#!pipe")

    files = r.cmdj('fj')[1:-1]
    sym_name = 'sym.%s' % os.path.basename(input_path)

    for f in files:
        if f['name'] == sym_name:
            print 'importing %s into %s...' % (input_path, sym_name)
            size = os.path.getsize(input_path)
            if size != f['size']:
                diff = size - f['size']
                sign = '+'
                if diff < 0:
                    sign = ''
                r.cmd('r%s%d@%d' % (sign, diff, f['offset']))
                r.cmd('wff %s@%d' % (input_path, f['offset']))
            return

    print 'cannot find %s in ggpack, skipping' % sym_name

def import_ggdir (input_path):
    files = [os.path.join(input_path,f) for f in listdir(input_path) if os.path.isfile(os.path.join(input_path, f))]
    if len(files) == 0:
        print 'no files in %s' % input_path
    print 'importing %d files, go take a coffee now...' % len(files)
    for file_path in files:
        import_ggfile(file_path)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print 'please provide the input file or directory path'
    else:
        input_path = os.path.abspath(sys.argv[1])
        if os.path.isdir(input_path):
            import_ggdir(input_path)
        elif os.path.isfile(input_path):
            import_ggfile(input_path)

