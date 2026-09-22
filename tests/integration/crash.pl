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
# The fault points exercised here (restore/before, restore/after) run inside
# the heal/sync process itself, so they are deterministic.  FUSE-time points
# (create/*, update/*, delete/*) are available for manual investigation.
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

# Read the five little-endian 64-bit fields of a device's state.db:
# seq, mounted, working, restoring, degraded.  Returns undef on a malformed
# file so a test can report it explicitly.
sub state_fields {
    my ($dev)  = @_;
    my $data   = slurp("$dev/.muxfs/state.db");
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
    fail("working=$working not cleared on $dev ($tag)")   if $working   != 0;
    fail("restoring=$restoring not cleared on $dev ($tag)") if $restoring != 0;
    fail("degraded=$degraded on $dev ($tag)")             if $degraded  != 0;
    fail("mounted=$mounted on $dev ($tag)")               if $mounted   != 0;
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

# ---------------------------------------------------------------------------

make_path( $dev_a, $dev_b, $dev_c, $mp );

print "== format\n";
must_run( "format", $EMUXFS, "format", "-a", "md5", $dev_a, $dev_b )
  or exit 1;

print "== populate\n";
must_run( "mount", $EMUXFS, "mount", $mp, $dev_a, $dev_b ) or exit 1;
$mounted = 1;
for ( my $i = 0 ; $i < 100 && !-e $mp ; $i++ ) {
    select( undef, undef, undef, 0.01 );
}
put( "$mp/r", "original\n" ) or fail("write r");
must_run( "mkdir d", "mkdir", "$mp/d" );
put( "$mp/d/n", "nested\n" ) or fail("write nested");
must_run( "umount", "umount", $mp );
$mounted = 0;

# Corrupt both a file and a nested file on one mirror.
sub corrupt {
    my ($label) = @_;
    my $evil = sprintf( "evil-%s\n", $label );
    put( "$dev_a/r",   $evil ) or fail("corrupt r");
    put( "$dev_a/d/n", $evil ) or fail("corrupt n");
}

sub recover_and_check {
    my ($tag) = @_;
    must_run( "recovery heal $tag",        $EMUXFS, "heal",  $dev_a, $dev_b );
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

if ( $failures != 0 ) {
    print STDERR "$failures crash test(s) failed\n";
    exit 1;
}
print "All crash tests passed\n";
exit 0;
