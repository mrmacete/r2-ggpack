import sys

"""
Usage: python ./make_pass2_table.py input_file_name symbol_name

Where input_file_name is a file generated from r2 like this:
    pc 0x1000@OFFSET!0x1000 > input_file_name

where OFFSET is the address at which the XOR obfuscation table
is located inside your TWP executable.

symbol_name can be anything, it's suggested to call it PASS2_Tn,
where "n" is a sequence number.
"""


def transform (input_file_name, sym_name):
    upper_sym_name = sym_name.upper()
    output_file_name = 'r_ggpack_%s.h' % sym_name.lower()
    ln = 0
    with open(input_file_name) as fi, open(output_file_name, 'w') as fo:
        fo.write('#ifndef R_GGPACK_%s_H\n#define R_GGPACK_%s_H\n' % (upper_sym_name, upper_sym_name))
        fo.write('#define R_GGPACK_XOR_%s {\\\n' % upper_sym_name)

        for line in fi:
            if ln < 2:
                ln += 1
                continue
            if line.find('};') == 0:
                fo.write('}\n')
            else:
                fo.write('%s\\\n' % line.strip())

        fo.write('#endif\n')

if __name__ == '__main__':
    transform(sys.argv[1], sys.argv[2])


