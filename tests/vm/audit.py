#!/usr/bin/env python3
import signal
import traceback

import selfcontained as s


def main():
    s.safety()
    signal.signal(signal.SIGALRM, s.io_deadline)
    s.control_abi_guards()
    s.kernel_namespace_guard()
    s.tree_index_budget_guard()
    with s.lower_filesystem('ext4') as lower:
        s.fenced_detach_guard(lower)
    s.command('sync')
    print('DYNBLK AUDIT PASS', flush=True)


if __name__ == '__main__':
    try:
        main()
    except BaseException:
        traceback.print_exc()
        s.kernel_log('dmesg-failure.txt')
        s.command('sync')
        print('DYNBLK FAIL audit', flush=True)
        raise
