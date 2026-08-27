import os, json, subprocess
from pathlib import Path
from optparse import OptionParser
from common import home_to_tilde

HOME = os.path.expanduser("~")
P4DEF_FILE = str(Path(__file__).resolve().parent / "p4_def.txt")
target_files = set()

def home_to_tilde(path: str) -> str:
    path = str(path)
    if path == str(HOME):
        return "~"
    if path.startswith(str(HOME) + "/"):
        return "~" + path[len(str(HOME)):]
    return path

def clear_count_file_set():
    global target_files
    target_files = set()

def get_const_entries(filename):
    const_entry_line_cnt = 0
    with open(filename, 'r') as fp:
        begin_const_entry = False
        pause_const_entry = False
        for count, line in enumerate(fp):
            l = line.strip()
            words = l.split()
            if len(words) >= 3 and words[0] == 'const' and words[1].startswith('entries'):
                const_entry_line_cnt += 1
                begin_const_entry = True
                pause_const_entry = False
                continue

            if not begin_const_entry:
                continue

            if l.startswith('//'):
                continue

            if l.startswith('{'):
                const_entry_line_cnt += 1
                continue
            if l.startswith('/*'):
                pause_const_entry = True
                continue

            if pause_const_entry:
                if '*/' in l:
                    pause_const_entry = False
                    continue
                continue
            if '}' in l:
                const_entry_line_cnt += 1
                begin_const_entry = False
                pause_const_entry = False
                continue
            # Normal line in const entry
            const_entry_line_cnt += 1

    return const_entry_line_cnt

def get_count_file(filename, flags, verbose):
    filename = os.path.expanduser(filename)
    print(f"count {filename}")
    result = subprocess.run(
        ["cloc", f"--force-lang-def={P4DEF_FILE}", filename, "--json"],
        check=True,
        text=True,
        capture_output=True,
    )
    data = json.loads(result.stdout)

    p4_stats = data.get("P4", {})

    macro_line_cnt = 0
    with open(filename, 'r') as fp:
        do_macro_lines = False
        for count, line in enumerate(fp):
            if do_macro_lines:
                macro_line_cnt += 1
                if line.strip().startswith('#endif'):
                    do_macro_lines = False
                    continue

            if line.strip().startswith('#ifdef '):
                macro_txt = line.strip()[len('#ifdef'):].strip()
                if macro_txt not in flags:
                    do_macro_lines = True
                macro_line_cnt += 1

    const_entry_line_cnt = get_const_entries(filename)

    code_line_cnt = p4_stats.get("code", 0) - macro_line_cnt
    if verbose:
        print(f'{home_to_tilde(filename)}: code: {code_line_cnt} ', \
              f'comment: {p4_stats.get("comment", 0)} ', \
              f'blank: {p4_stats.get("blank", 0)} ', \
              f'macro: {macro_line_cnt} ', \
              f'const entry: {const_entry_line_cnt}')
    return code_line_cnt, code_line_cnt - const_entry_line_cnt

def get_include_files(file_name, flags, include_dirs, verbose):
    global target_files

    with open(file_name, 'r') as fp:
        do_ignore_lines = False
        for count, line in enumerate(fp):
            if line.strip().startswith('#ifdef '):
                macro_txt = line.strip()[len('#ifdef'):].strip()
                if macro_txt != 'WITH_UPF' and macro_txt != 'WITH_INT':
                    continue

                if macro_txt not in flags:
                    do_ignore_lines = True

            if line.strip().startswith('#endif'):
                do_ignore_lines = False

            if do_ignore_lines:
                continue

            if line.strip().startswith('#include "'):
                line_words = line.split('"')

                for include_dir in include_dirs:
                    file_name = f'{include_dir}/{line_words[1]}'
                    file_name = os.path.expanduser(file_name)
                    if os.path.isfile(file_name):
                        include_file = os.path.abspath(file_name)
                        if include_file not in target_files:
                            if verbose:
                                print(f'Includes {line_words[1]}')
                            target_files.add(include_file)
                            get_include_files(include_file, flags, include_dirs, verbose)

def count_files(inputfile, flags, include_dirs=[], verbose=True):
    global target_files

    inputfile = os.path.expanduser(inputfile)
    include_dirs.append(Path(inputfile).parent)
    get_include_files(inputfile, flags, include_dirs, verbose)
    target_files.add(inputfile)

    total_counts = 0
    total_counts_without_const_entry = 0
    for target_file in target_files:
        counts, counts_without_const_entry = get_count_file(target_file, flags, verbose)
        total_counts += counts
        total_counts_without_const_entry += counts_without_const_entry

    if verbose:
        print(f'Total:', total_counts)
        print(f'Total without const entry:', total_counts_without_const_entry)

    return total_counts, total_counts_without_const_entry

if __name__ == '__main__':
    parser = OptionParser()
    parser.add_option("-i", "--input", dest="input",
            help="input P4 file to count", metavar="FILE")
    parser.add_option("-I", action="append", type="string",
            dest="include_dirs", metavar="DIR", default=[],
            help="specify include directories")
    parser.add_option("-D", action="append", type="string",
            dest="flags", metavar="FLAG", default=[],
            help="add flags")

    (options, args) = parser.parse_args()

    if options.input is None:
        parser.error("option -i/--input is required")

    count_files(options.input, options.flags, options.include_dirs)

