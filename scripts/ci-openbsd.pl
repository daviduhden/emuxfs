#!/usr/bin/env perl
# Shared validation inside the disposable OpenBSD CI VM.
use strict;
use warnings;
use Cwd   qw(getcwd);
use POSIX ();

sub run {
    my (@command) = @_;
    system { $command[0] } @command;
    die "FAIL: @command (wait status $?)\n" if $? != 0;
    return;
}

# Capture both streams without a shell; retain raw status including signals.
sub capture {
    my (@command) = @_;
    my $pid       = open my $output, '-|';
    die "FAIL: fork: $!\n" unless defined $pid;
    if ( $pid == 0 ) {
        open STDERR, '>&', \*STDOUT or POSIX::_exit(127);
        exec { $command[0] } @command or POSIX::_exit(127);
    }
    my @lines = <$output>;
    close $output;
    my $status = $?;
    return ( \@lines, $status );
}

sub require_fuse {
    die "FAIL: CI requires /dev/fuse0\n" unless -c '/dev/fuse0';
    return;
}

eval {
    die "FAIL: CI requires root\n" unless $> == 0;
    require_fuse();
    my $workspace = $ENV{GITHUB_WORKSPACE};
    die "FAIL: GITHUB_WORKSPACE is required\n"
      unless defined($workspace) && length($workspace);
    chdir $workspace or die "FAIL: chdir $workspace: $!\n";
    my $root = getcwd();

    print "== Build (clang, C23) ==\n";
    run( 'make', 'clean' );
    run('make');
    run( './emuxfs', 'version' );
    print "== Strict warnings (clang, full set, -Werror) ==\n";
    run( 'make', 'check' );
    print "== Unit tests ==\n";
    run( 'make', 'test' );
    run('./tests/unit/test_core');

    print "== FUSE integration tests ==\n";
    require_fuse();
    run('make');
    {
        local $ENV{EMUXFS} = "$root/emuxfs";
        my ( $lines, $status ) =
          capture( $^X, 'tests/integration/integration.pl' );
        print @{$lines};
        die "FAIL: integration reported failures\n"
          if $status != 0 || grep { /\AFAIL:/x } @{$lines};
    }
    print "== Parallel-load stress test (32 workers) ==\n";
    require_fuse();
    {
        # Preserve the existing emulated-arm64 deadline and 32x20 workload.
        local $ENV{PARALLEL_TIMEOUT} = 1800;
        local $ENV{EMUXFS}           = "$root/emuxfs";
        run( $^X, 'tests/integration/parallel.pl' );
    }
    print "== Fault-injection tests ==\n";
    run( 'make', 'faultbuild' );
    {
        local $ENV{EMUXFS}       = "$root/emuxfs";
        local $ENV{EMUXFS_FAULT} = "$root/emuxfs-fault";
        run( $^X, 'tests/integration/crash.pl' );
    }
    print "== Bounded parser fuzzing ==\n";
    run( 'make', 'fuzz-smoke' );
    print "== Install ==\n";
    run( 'make', 'install' );
    die "FAIL: installed binary missing\n" unless -x '/usr/local/sbin/emuxfs';
    die "FAIL: installed manual missing\n"
      unless -f '/usr/local/man/man1/emuxfs.1';

    print "== Manual page ==\n";
    {
        # STYLE and missing host-manual references remain non-fatal.
        my ( $lines, $status ) = capture( 'mandoc', '-T', 'lint', 'emuxfs.1' );
        my @errors = grep { /ERROR|WARNING/x } @{$lines};
        if (@errors) {
            print @errors;
            die "FAIL: mandoc reported errors or warnings\n";
        }
        die "FAIL: cannot execute mandoc\n"
          if $status == ( 127 << 8 ) || ( $status & 127 );
    }
    {
        local $ENV{MANWIDTH} = 80;
        my ( $lines, $status ) =
          capture( 'man', '-M', '/usr/local/man', 'emuxfs' );
        die "FAIL: installed manual cannot be rendered (wait status $status)\n"
          if $status != 0;
    }
    print "== Linkage ==\n";
    run( 'ldd', './emuxfs' );
    print "== Fallback allocator build and unit tests ==\n";
    run( 'make', 'clean' );
    run( 'make', 'EMUXFS_DS_MALLOC=1', 'unittest' );
    run('./tests/unit/test_core');
    print "== Clean rebuild ==\n";
    run( 'make', 'clean' );
    run('make');
    print "All CI steps passed.\n";
    1;
} or do {
    print {*STDERR} $@;
    exit 1;
};
