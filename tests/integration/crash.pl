#!/usr/bin/env perl
#
# Crash-consistency / fault-injection tests for emuxfs.
#
# Requires two binaries:
#   EMUXFS        the normal build
#   EMUXFS_FAULT  a build made with -DEMUXFS_FAULT_INJECTION (see
#                 'make faultbuild'), which terminates at a named boundary
#                 when EMUXFS_FAULT_POINT names it.
#
# The fault points exercised here run in two places: inside the heal/sync
# process itself (restore/before, restore/after) and inside FUSE callbacks
# (create/after_meta, update/after_meta, delete/after_meta).  In both cases the
# interrupted device is recovered with sync and the result is audited.
#
# Everything happens inside a private temporary sandbox.  See TESTING.md.

use strict;
use warnings;

use Cwd           qw(getcwd);
use File::Compare qw(compare);
use File::Path    qw(make_path remove_tree);
use File::Temp    qw(tempdir);

my $EMUXFS       = $ENV{EMUXFS}       // ( getcwd() . "/emuxfs" );
my $EMUXFS_FAULT = $ENV{EMUXFS_FAULT} // ( getcwd() . "/emuxfs-fault" );
my $FAULT_STATUS = 70;

foreach my $bin ( $EMUXFS, $EMUXFS_FAULT ) {
    unless ( -x $bin ) {
        print STDERR "SKIP: missing binary $bin\n";
        exit 0;
    }
}

if ( $> != 0 ) {
    print STDERR "SKIP: crash tests require root\n";
    exit 0;
}
unless ( -c "/dev/fuse0" ) {
    print STDERR "SKIP: /dev/fuse0 is not available\n";
    exit 0;
}

my $tmpbase = $ENV{TMPDIR} // "/tmp";
$tmpbase =~ s{/+$}{};

my $sandbox = tempdir( "emuxfs-crash.XXXXXX", DIR => $tmpbase, CLEANUP => 0 );
unless ( $sandbox =~ m{\A\Q$tmpbase\E/[^/]+\z} ) {
    print STDERR "Refusing to run: sandbox '$sandbox' is outside '$tmpbase'\n";
    exit 2;
}

my $dev_a    = "$sandbox/dev_a";
my $dev_b    = "$sandbox/dev_b";
my $dev_c    = "$sandbox/dev_c";
my $mp       = "$sandbox/mp";
my $mounted  = 0;
my $failures = 0;

sub fail {
    my ($msg) = @_;
    print STDERR "FAIL: $msg\n";
    $failures++;
}

sub in_sandbox {
    my ($path) = @_;
    return defined($path) && $path =~ m{\A\Q$tmpbase\E/[^/]};
}

my $cleaned = 0;

sub cleanup {
    return if $cleaned++;
    if ($mounted) {
        system( "umount", $mp );
        $mounted = 0;
    }
    if ( defined($sandbox) && in_sandbox($sandbox) ) {
        my $err;
        remove_tree( $sandbox, { safe => 1, error => \$err } );
    }
    else {
        print STDERR "Refusing to remove '" . ( $sandbox // "undef" ) . "'\n";
    }
}
END { cleanup() }
$SIG{INT} = $SIG{TERM} = sub { cleanup(); exit 130 };

sub run {
    my (@cmd) = @_;
    system(@cmd);
    return $?;
}

sub must_run {
    my ( $what, @cmd ) = @_;
    my $st = run(@cmd);
    if ( $st != 0 ) {
        fail( "$what failed (exit " . ( $st >> 8 ) . "): @cmd" );
        return 0;
    }
    return 1;
}

# Write 'content' to 'path', creating or truncating it.  Returns the write
# result so callers can report a failure with the path in the message.
sub put {
    my ( $path, $content ) = @_;
    open( my $fh, ">", $path ) or do {
        fail("cannot write $path: $!");
        return 0;
    };
    print $fh $content;
    close($fh);
    return 1;
}

sub slurp {
    my ($path) = @_;
    open( my $fh, "<", $path ) or return undef;
    local $/;
    my $data = <$fh>;
    close($fh);
    return $data;
}

# The FUSE daemon clears 'mounted' in state.db asynchronously after umount(8);
# wait for the device to be clean before running audit/heal/sync.
sub wait_state_clean {
    my ($dev) = @_;
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $db = slurp("$dev/.muxfs/state.db");
        if ( defined($db) && length($db) == 40 ) {
            my @f = unpack( "Q<5", $db );
            return 1 if $f[1] == 0 && $f[2] == 0 && $f[3] == 0;
        }
        select( undef, undef, undef, 0.02 );
    }
    return 0;
}

# Read the five little-endian 64-bit fields of a device's state.db:
# seq, mounted, working, restoring, degraded.  Returns undef on a malformed
# file so a test can report it explicitly.
sub state_fields {
    my ($dev) = @_;
    my $data = slurp("$dev/.muxfs/state.db");
    return undef
      unless defined($data) && length($data) == 5 * 8;
    return [ unpack( "Q<5", $data ) ];
}

sub assert_clean {
    my ( $dev, $tag ) = @_;
    my $st = state_fields($dev);
    if ( !defined($st) ) {
        fail("cannot read state.db for $dev ($tag)");
        return;
    }
    my ( $seq, $mounted, $working, $restoring, $degraded ) = @$st;
    fail("working=$working not cleared on $dev ($tag)")     if $working != 0;
    fail("restoring=$restoring not cleared on $dev ($tag)") if $restoring != 0;
    fail("degraded=$degraded on $dev ($tag)")               if $degraded != 0;
    fail("mounted=$mounted on $dev ($tag)")                 if $mounted != 0;
}

# Run a command with a fault point armed, then disarm it.  Returns the exit
# status (the high byte of $?).
sub run_fault {
    my ( $point, @cmd ) = @_;
    $ENV{EMUXFS_FAULT_POINT} = $point;
    system(@cmd);
    my $st = $?;
    delete $ENV{EMUXFS_FAULT_POINT};
    return $st >> 8;
}

# Write or create a file, ignoring errors: the operation under test is
# expected to fail when the daemon is terminated part-way through it.
sub try_put {
    my ( $path, $content ) = @_;
    open( my $fh, ">", $path ) or return;
    print $fh $content;
    close($fh);
}

# Mount the array in the foreground as a background shell job so that a fault
# point inside a FUSE callback terminates that daemon.  With $point undef the
# normal binary is used.
sub fuse_mount_bg {
    my ( $a, $b, $point, $log ) = @_;
    my $bin = defined($point) ? $EMUXFS_FAULT                  : $EMUXFS;
    my $env = defined($point) ? "EMUXFS_FAULT_POINT='$point' " : "";

    # Remove any previous log so readiness below cannot be satisfied by a
    # stale "entering fuse_loop" line from an earlier mount.
    unlink($log);
    system("EMUXFS_TRACE=1 $env$bin mount -f $mp $a $b >'$log' 2>&1 &");
    $mounted = 1;

    # Wait until the daemon has entered its event loop.
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $l = slurp($log);
        last if defined($l) && $l =~ /entering fuse_loop/;
        select( undef, undef, undef, 0.02 );
    }
}

# Unmount the foreground job.  After a fault the daemon has already exited and
# the kernel detaches the filesystem, so both forms are attempted and the
# failure of the second is expected and ignored.
sub fuse_umount {
    system("umount $mp >/dev/null 2>&1");
    system("umount -f $mp >/dev/null 2>&1");
    $mounted = 0;
}

# Format a fresh pair, populate f=v0 through a normal mount, then interrupt an
# operation on dev a at $point and recover the array with sync.  $trigger
# performs the interrupted operation and $verify checks the rolled-back tree.
sub fuse_fault_recover {
    my ( $point, $trigger, $verify, $tag ) = @_;
    my $a   = "$sandbox/ff_${tag}_a";
    my $b   = "$sandbox/ff_${tag}_b";
    my $log = "$sandbox/ff_${tag}.log";

    remove_tree( $a, $b );
    make_path( $a, $b );
    return
      unless must_run( "format ($tag)", $EMUXFS, "format", "-a", "md5", $a,
        $b );

    fuse_mount_bg( $a, $b, undef, $log );
    put( "$mp/f", "v0\n" ) or fail("populate f ($tag)");
    fuse_umount();
    wait_state_clean($a) or fail("$a not clean before fault ($tag)");
    wait_state_clean($b) or fail("$b not clean before fault ($tag)");

    fuse_mount_bg( $a, $b, $point, $log );
    $trigger->();
    select( undef, undef, undef, 1.0 );    # let the daemon die and detach
    fuse_umount();

    must_run( "sync ($tag)",  $EMUXFS, "sync",  $a, $b );
    must_run( "audit ($tag)", $EMUXFS, "audit", $a, $b );
    $verify->( $a, $b );
    assert_clean( $a, $tag );
    assert_clean( $b, $tag );
}

# ---------------------------------------------------------------------------

make_path( $dev_a, $dev_b, $dev_c, $mp );

print "== format\n";
must_run( "format", $EMUXFS, "format", "-a", "md5", $dev_a, $dev_b )
  or exit 1;

print "== populate\n";
must_run( "mount", $EMUXFS, "mount", $mp, $dev_a, $dev_b ) or exit 1;
$mounted = 1;

# Wait until the daemon is actually serving FUSE requests (the mount point
# returns ENXIO until then).
for ( my $i = 0 ; $i < 500 ; $i++ ) {
    last if stat($mp);
    select( undef, undef, undef, 0.02 );
}
put( "$mp/r", "original\n" ) or fail("write r");
must_run( "mkdir d", "mkdir", "$mp/d" );
put( "$mp/d/n", "nested\n" ) or fail("write nested");
must_run( "umount", "umount", $mp );
$mounted = 0;
wait_state_clean($dev_a) or fail("dev_a not clean after umount");
wait_state_clean($dev_b) or fail("dev_b not clean after umount");

# Corrupt both a file and a nested file on one mirror.
sub corrupt {
    my ($label) = @_;
    my $evil = sprintf( "evil-%s\n", $label );
    put( "$dev_a/r",   $evil ) or fail("corrupt r");
    put( "$dev_a/d/n", $evil ) or fail("corrupt n");
}

sub recover_and_check {
    my ($tag) = @_;

    # The interrupted heal left dev_a with restoring=1, so heal itself refuses
    # to open it (fail closed).  The explicit recovery for an interrupted
    # device is sync, which force-opens the destination and rebuilds it.
    must_run( "recovery sync $tag",        $EMUXFS, "sync",  $dev_a, $dev_b );
    must_run( "audit after recovery $tag", $EMUXFS, "audit", $dev_a, $dev_b );
    fail("r did not converge ($tag)") if compare( "$dev_a/r", "$dev_b/r" ) != 0;
    fail("n did not converge ($tag)")
      if compare( "$dev_a/d/n", "$dev_b/d/n" ) != 0;
    fail("r content wrong ($tag)")
      unless ( slurp("$dev_a/r") // "" ) eq "original\n";
    fail("n content wrong ($tag)")
      unless ( slurp("$dev_a/d/n") // "" ) eq "nested\n";
}

print "== fault at restore/before\n";
corrupt("before");
my $rc = run_fault( "restore/before", $EMUXFS_FAULT, "heal", $dev_a, $dev_b );
fail("expected exit $FAULT_STATUS, got $rc") unless $rc == $FAULT_STATUS;
recover_and_check("before");

print "== fault at restore/after\n";
corrupt("after");
$rc = run_fault( "restore/after", $EMUXFS_FAULT, "heal", $dev_a, $dev_b );
fail("expected exit $FAULT_STATUS, got $rc") unless $rc == $FAULT_STATUS;
recover_and_check("after");

print "== fault during sync\n";
$rc = run_fault( "restore/before", $EMUXFS_FAULT, "sync", $dev_c, $dev_a );
fail("expected exit $FAULT_STATUS from sync, got $rc")
  unless $rc == $FAULT_STATUS;

# The destination is partially populated; a second, uninterrupted sync must
# converge it.
must_run( "recovery sync", $EMUXFS, "sync", $dev_c, $dev_a );
fail("sync did not converge") if compare( "$dev_c/r", "$dev_a/r" ) != 0;
fail("sync did not converge (n)")
  if compare( "$dev_c/d/n", "$dev_a/d/n" ) != 0;
assert_clean( $dev_c, "after sync recovery" );

# Plant an interrupted-operation marker (working=1) on the destination, as a
# power loss would leave it, and verify that sync clears it so the array can be
# mounted again.  This is the documented post-power-loss recovery path.
print "== sync clears a planted interrupted-operation marker\n";
{
    my $st = state_fields($dev_c);
    if ( !defined($st) ) {
        fail("cannot read state.db for $dev_c before planting");
    }
    else {
        my $seq = $st->[0];
        if ( open( my $fh, ">", "$dev_c/.muxfs/state.db" ) ) {
            binmode($fh);
            print $fh pack( "Q<5", $seq, 0, 1, 0, 0 );
            close($fh);
        }
        else {
            fail("cannot plant interrupted marker: $!");
        }
    }
}
must_run( "sync after planted marker", $EMUXFS, "sync", $dev_c, $dev_a );
assert_clean( $dev_c, "after planted marker" );

# ---------------------------------------------------------------------------
# Faults inside FUSE callbacks.  An interrupted create/update/delete leaves the
# first device with working=1 and a partially committed record; the explicit
# recovery is sync, which rebuilds that device from the intact mirror and thus
# rolls the uncommitted operation back.

print "== fault at create/after_meta (FUSE)\n";
fuse_fault_recover(
    "create/after_meta",
    sub { try_put( "$mp/new", "x\n" ); },
    sub {
        my ( $a, $b ) = @_;
        fail("create not rolled back") if -e "$a/new" || -e "$b/new";
        fail("f changed by failed create")
          unless ( slurp("$a/f") // "" ) eq "v0\n";
        fail("array did not converge after create fault")
          if compare( "$a/f", "$b/f" ) != 0;
    },
    "create"
);

print "== fault at update/after_meta (FUSE)\n";
fuse_fault_recover(
    "update/after_meta",
    sub { try_put( "$mp/f", "v1\n" ); },
    sub {
        my ( $a, $b ) = @_;
        fail("update not rolled back")
          unless ( slurp("$a/f") // "" ) eq "v0\n";
        fail("array did not converge after update fault")
          if compare( "$a/f", "$b/f" ) != 0;
    },
    "update"
);

print "== fault at delete/after_meta (FUSE)\n";
fuse_fault_recover(
    "delete/after_meta",
    sub { unlink("$mp/f"); },
    sub {
        my ( $a, $b ) = @_;
        fail("delete not rolled back") unless -f "$a/f";
        fail("array did not converge after delete fault")
          if compare( "$a/f", "$b/f" ) != 0;
    },
    "delete"
);

if ( $failures != 0 ) {
    print STDERR "$failures crash test(s) failed\n";
    exit 1;
}
print "All crash tests passed\n";
exit 0;
