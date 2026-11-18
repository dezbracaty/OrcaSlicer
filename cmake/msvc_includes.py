"""Translate localized MSVC /showIncludes lines for Ninja's UTF-8 parser."""
import argparse
import ctypes
import os
import subprocess
import sys

CANONICAL = b'Note: including file: '


def encodings():
    values = ['utf-8']
    if os.name == 'nt':
        values += [f'cp{ctypes.windll.kernel32.GetACP()}',
                   f'cp{ctypes.windll.kernel32.GetOEMCP()}']
    return list(dict.fromkeys(values + ['cp936']))


def prefixes(detected):
    # CMake's compiler probe can itself have decoded UTF-8 as the system ACP.
    texts = [detected.strip(), 'Note: including file:', '\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:']
    for encoding in encodings():
        try:
            texts.append(detected.encode(encoding).decode('utf-8').strip())
        except UnicodeError:
            pass
    result = []
    for value in texts:
        if not value:
            continue
        for encoding in encodings():
            try:
                pair = (value.encode(encoding), encoding)
                if pair not in result:
                    result.append(pair)
            except UnicodeError:
                pass
    return result


def normalize(line, markers):
    for marker, encoding in markers:
        if line.startswith(marker):
            raw = line[len(marker):].lstrip(b' ').rstrip(b'\r\n')
            # ASCII prefixes carry no clue about the path's encoding.
            candidates = list(dict.fromkeys([encoding] + encodings()))
            decoded = []
            for candidate in candidates:
                try:
                    decoded.append(raw.decode(candidate))
                except UnicodeError:
                    pass
            if not decoded:
                return line
            path = next((p for p in decoded if os.path.isfile(p)), decoded[0])
            return CANONICAL + path.encode('utf-8') + b'\n'
    return line


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', default='')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('missing compiler command')
    markers = prefixes(args.prefix)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in process.stdout:
        sys.stdout.buffer.write(normalize(line, markers))
        sys.stdout.buffer.flush()
    return process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
