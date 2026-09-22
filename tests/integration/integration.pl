#!/usr/bin/env perl
#
# Integration tests for emuxfs.
#
# These tests mount a real FUSE filesystem, so they require root and a working
# /dev/fuse0.  Everything is created inside a private temporary sandbox; the
# script refuses to run unless it can prove that the sandbox is inside the
# intended temporary base.  No pre-existing path is ever read, written or
# removed.
#
# See TESTING.md.

use strict;
use warnings;

use Cwd           qw(getcwd);
use File::Compare qw(compare);
use File::Copy    qw(copy);
use File::Path    qw(make_path remove_tree);
use File::Temp    qw(tempdir);
use POSIX         qw(WNOHANG);

my $EMUXFS = $ENV{EMUXFS} // ( getcwd() . "/emuxfs" );

unless ( -x $EMUXFS ) {
    print STDERR "emuxfs binary not found at $EMUXFS (set EMUXFS=...)\n";
    exit 2;
}

if ( $> != 0 ) {
    print STDERR "SKIP: integration tests require root\n";
    exit 0;
}

# Create the FUSE device node if the image did not populate /dev.
unless ( -c "/dev/fuse0" ) {
    system( "sh", "-c", "cd /dev && MAKEDEV fuse >/dev/null 2>&1" );
}
unless ( -c "/dev/fuse0" ) {
    print STDERR "SKIP: /dev/fuse0 is not available\n";
    exit 0;
}

my $tmpbase = $ENV{TMPDIR} // "/tmp";
$tmpbase =~ s{/+$}{};

# tempdir() appends the template; the result is a fresh directory.
my $sandbox = tempdir( "emuxfs-test.XXXXXX", DIR => $tmpbase, CLEANUP => 0 );

# Prove we are inside the intended temporary base before doing anything else.
# \Q...\E quotes the base so regex metacharacters in the path cannot widen it.
unless ( $sandbox =~ m{\A\Q$tmpbase\E/[^/]+\z} ) {
    print STDERR "Refusing to run: sandbox '$sandbox' is outside '$tmpbase'\n";
    exit 2;
}

my $dev_a    = "$sandbox/dev_a";
my $dev_b    = "$sandbox/dev_b";
my $dev_c    = "$sandbox/dev_c";
my $mp       = "$sandbox/mp";
my $work     = "$sandbox/work";
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

    # Re-prove the sandbox before removing anything.
    if ( defined($sandbox) && in_sandbox($sandbox) ) {
        my $err;
        remove_tree( $sandbox, { safe => 1, error => \$err } );
        if ( $err && @$err ) {
            print STDERR "cleanup: could not remove $sandbox\n";
        }
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
        my $code = $st >> 8;
        fail("$what failed (exit $code): @cmd");
        return 0;
    }
    return 1;
}

# Capture the output of a command as chomped lines plus its raw wait status.
sub capture {
    my (@cmd) = @_;
    open( my $fh, "-|", @cmd ) or do {
        fail("cannot execute @cmd: $!");
        return ( [], -1 );
    };
    my @lines = <$fh>;
    close($fh);
    my $st = $?;
    chomp @lines;
    return ( \@lines, $st );
}

# Read a whole file as one scalar; Perl's chomp/length/substr give the caller
# precise control over the exact bytes.
sub slurp {
    my ($path) = @_;
    open( my $fh, "<", $path ) or return undef;
    local $/;
    my $data = <$fh>;
    close($fh);
    return $data;
}

sub subdir {
    my ( $base, @parts ) = @_;
    return join( "/", $base, @parts );
}

# The FUSE daemon clears 'mounted' in state.db asynchronously, after umount(8)
# returns.  Wait until the device is clean so that a following command (audit,
# heal, mount) does not race the daemon's teardown.
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

sub state_mounted {
    my ($dev) = @_;
    my $db = slurp("$dev/.muxfs/state.db");
    return -1 unless defined($db) && length($db) == 40;
    my @f = unpack( "Q<5", $db );
    return $f[1];
}

sub mount_array {
    my $mlog = "$sandbox/mount-fg.log";

    # Remove any previous log so readiness below cannot be satisfied by a
    # stale "entering fuse_loop" line from an earlier mount.
    unlink($mlog);

    # Run in the foreground (as a background shell job) so the daemon's
    # standard error, including the trace output, is captured.
    system("EMUXFS_TRACE=1 $EMUXFS mount -f $mp $dev_a $dev_b >'$mlog' 2>&1 &");
    $mounted = 1;

    # Wait until the daemon has entered its event loop.
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $l = slurp($mlog);
        last if defined($l) && $l =~ /entering fuse_loop/;
        select( undef, undef, undef, 0.02 );
    }
    return 1;
}

sub unmount_array {
    must_run( "umount", "umount", $mp );
    $mounted = 0;
    my $clean_a = wait_state_clean($dev_a);
    my $clean_b = wait_state_clean($dev_b);
    if ( !$clean_a || !$clean_b ) {
        fail("dev_a not clean after umount") unless $clean_a;
        fail("dev_b not clean after umount") unless $clean_b;
        my $l = slurp("$sandbox/mount-fg.log");
        if ( defined($l) ) {
            my @ln = split( /\n/, $l );
            my @t  = @ln > 200 ? @ln[ -200 .. -1 ] : @ln;
            print "diag: daemon trace tail:\n", join( "\n", @t ), "\n";
        }
        system("ps -ax");
    }
}

# ---------------------------------------------------------------------------

make_path( $dev_a, $dev_b, $dev_c, $mp, $work );

print "== format\n";
must_run( "format", $EMUXFS, "format", "-a", "sha1", $dev_a, $dev_b )
  or exit 1;

print "== basic operations\n";
mount_array() or exit 1;

must_run( "touch",   "touch", "$mp/reg" );
must_run( "mkdir",   "mkdir", "$mp/dir" );
must_run( "symlink", "ln",    "-s", "target", "$mp/lnk" );
fail("reg not mirrored to a") unless -f "$dev_a/reg";
fail("reg not mirrored to b") unless -f "$dev_b/reg";
fail("dir not mirrored")      unless -d "$dev_a/dir";
fail("lnk not mirrored")      unless -l "$dev_a/lnk";

open( my $rfh, ">", "$mp/reg" ) or fail("write reg: $!");
print $rfh "foo\n";
close($rfh);
fail("read-back content") unless ( slurp("$mp/reg")    // "" ) eq "foo\n";
fail("mirror a content")  unless ( slurp("$dev_a/reg") // "" ) eq "foo\n";
fail("mirror b content")  unless ( slurp("$dev_b/reg") // "" ) eq "foo\n";

# A second daemon must never mount an array that is already mounted: the
# 'mounted' flag in state.db is the single-writer guard.
print "== a second mount of the same array is refused\n";
{
    my $mp2 = "$sandbox/mp2";
    must_run( "mkdir mp2", "mkdir", $mp2 );
    my $pid = fork();
    if ( !defined $pid ) {
        fail("fork: $!");
    }
    elsif ( $pid == 0 ) {
        open( STDOUT, ">", "$sandbox/mp2.log" );
        open( STDERR, ">&STDOUT" );
        exec( $EMUXFS, "mount", "-f", $mp2, $dev_a, $dev_b );
        exit 127;
    }
    else {

        # Wait for the refusal; if it instead mounted, that is the failure.
        my $reaped = 0;
        for ( my $i = 0 ; $i < 250 ; $i++ ) {
            if ( waitpid( $pid, WNOHANG ) == $pid ) { $reaped = 1; last; }
            select( undef, undef, undef, 0.02 );
        }
        if ( !$reaped ) {
            fail("a second mount of a mounted array was not refused");
            kill( "TERM", $pid );
            waitpid( $pid, 0 );
            system("umount -f $mp2 >/dev/null 2>&1");
        }
    }
    fail("first mount damaged by the refused second mount")
      unless ( slurp("$mp/reg") // "" ) eq "foo\n";
}

open( my $tfh, ">", "$mp/reg" ) or fail("truncate reg: $!");
close($tfh);
fail("truncate left content") if -s "$mp/reg";

must_run( "rename", "mv", "$mp/reg", "$mp/reg2" );
fail("rename not mirrored") unless -f "$dev_a/reg2";

must_run( "unlink", "rm", "$mp/reg2" );
fail("unlink not mirrored") if -e "$dev_a/reg2";
must_run( "rmdir", "rmdir", "$mp/dir" );
must_run( "symlink retarget", "ln", "-sf", "other", "$mp/lnk" );
my $target = readlink("$mp/lnk");
$target = "" unless defined $target;
fail("readlink after retarget") unless $target eq "other";

print "== modes are mirrored\n";
{
    open( my $mfh, ">", "$mp/mode" ) or fail("create mode: $!");
    print $mfh "mode\n";
    close($mfh);
    must_run( "chmod 0640", "chmod", "0640", "$mp/mode" );
    run("sync");
    my @a = stat("$dev_a/mode");
    my @b = stat("$dev_b/mode");
    fail("cannot stat mode copies") unless @a && @b;
    fail("mode not mirrored to a") unless ( $a[2] & 07777 ) == 0640;
    fail("mode not mirrored to b") unless ( $b[2] & 07777 ) == 0640;
}

print "== directory rename\n";
{
    must_run( "mkdir z", "mkdir", "$mp/z" );
    open( my $zfh, ">", "$mp/z/f" ) or fail("create z/f: $!");
    print $zfh "z\n";
    close($zfh);
    must_run( "mv z z2", "mv", "$mp/z", "$mp/z2" );
    fail("dir rename not mirrored a")
      unless -f "$dev_a/z2/f" && !-e "$dev_a/z";
    fail("dir rename not mirrored b") unless -f "$dev_b/z2/f";
    fail("dir rename content") unless ( slurp("$mp/z2/f") // "" ) eq "z\n";
}

print "== boundary sizes\n";
foreach my $sz ( 0, 1, 4095, 4096, 4097, 8191, 8192, 8193, 65537 ) {
    my $src = "$work/sz";
    must_run( "dd $sz", "dd", "if=/dev/urandom", "of=$src", "bs=1",
        "count=$sz" );
    must_run( "copy size $sz", "cp", $src, "$mp/sz" );
    run("sync");
    fail("mirror a mismatch size $sz") if compare( $src, "$dev_a/sz" ) != 0;
    fail("mirror b mismatch size $sz") if compare( $src, "$dev_b/sz" ) != 0;
    fail("read mismatch size $sz")     if compare( $src, "$mp/sz" ) != 0;
    must_run( "remove size $sz", "rm", "-f", "$mp/sz" );
}

print "== deep tree\n";
my $deep = $mp;
for ( my $i = 0 ; $i < 40 ; $i++ ) {
    $deep = subdir( $deep, "d$i" );
    must_run( "deep mkdir $i", "mkdir", $deep );
}
open( my $dfh, ">", "$deep/leaf" ) or fail("deep leaf: $!");
print $dfh "deep\n";
close($dfh);
fail("deep leaf content") unless ( slurp("$deep/leaf") // "" ) eq "deep\n";

print "== odd but valid names\n";
foreach my $name ( 'with space', 'a.b.c', '-dash', "\x{fc}n\x{ef}code" ) {

    # Use Perl's sprintf-style length/equality rather than shell quoting.
    open( my $nfh, ">", "$mp/$name" ) or do {
        fail("create '$name': $!");
        next;
    };
    print $nfh "$name\n";
    close($nfh);
    my $got = slurp("$mp/$name");
    fail("read '$name'") unless defined($got) && $got eq "$name\n";
}

print "== self-healing on read (content corruption)\n";
open( my $hfh, ">", "$mp/healme" ) or fail("create healme: $!");
print $hfh "good\n";
close($hfh);
run("sync");
open( my $efh, ">", "$dev_a/healme" ) or fail("corrupt mirror a: $!");
print $efh "evil\n";
close($efh);
fail("self-heal read") unless ( slurp("$mp/healme") // "" ) eq "good\n";
fail("self-heal restored a")
  unless ( slurp("$dev_a/healme") // "" ) eq "good\n";

print "== self-healing on read (missing node)\n";
open( my $gfh, ">", "$mp/gone" ) or fail("create gone: $!");
print $gfh "gone\n";
close($gfh);
run("sync");
must_run( "remove mirror a copy", "rm", "-f", "$dev_a/gone" );
fail("restore read") unless ( slurp("$mp/gone") // "" ) eq "gone\n";
fail("missing node not restored") unless -f "$dev_a/gone";

# A repair must reproduce the source timestamps (they are not checksummed in
# format version 1, but the copy should still match).
print "== self-healing reproduces the source timestamps\n";
{
    open( my $sfh, ">", "$mp/stamp" ) or fail("create stamp: $!");
    print $sfh "stamped\n";
    close($sfh);
    run("sync");
    must_run( "set stamp time", "touch", "-t", "202001020304.05",
        "$mp/stamp" );
    run("sync");
    my @ref = stat("$dev_b/stamp");
    fail("cannot stat stamp on b") unless @ref;
    open( my $cfh, ">", "$dev_a/stamp" ) or fail("corrupt stamp: $!");
    print $cfh "evil\n";
    close($cfh);
    fail("stamp self-heal")
      unless ( slurp("$mp/stamp") // "" ) eq "stamped\n";
    my @got = stat("$dev_a/stamp");
    fail("cannot stat healed stamp") unless @got;
    fail("healed mtime not reproduced") if $got[9] != $ref[9];
}

print "== self-healing (corrupt symlink)\n";
must_run( "create slink",    "ln", "-s", "right", "$mp/slink" );
must_run( "break a slink",   "rm", "-f", "$dev_a/slink" );
must_run( "corrupt a slink", "ln", "-s", "wrong", "$dev_a/slink" );
my $sl = readlink("$mp/slink");
$sl = "" unless defined $sl;
fail("symlink self-heal") unless $sl eq "right";
$sl = readlink("$dev_a/slink");
$sl = "" unless defined $sl;
fail("symlink restored in a") unless $sl eq "right";

print "== unmount / remount\n";
unmount_array();
mount_array() or exit 1;
fail("content across remount") unless ( slurp("$mp/healme") // "" ) eq "good\n";
open( my $pfh, ">", "$mp/post" ) or fail("write after remount: $!");
print $pfh "post-remount\n";
close($pfh);
fail("post mirrored") unless -f "$dev_b/post";
unmount_array();

# Repeated mount/unmount cycles must preserve the tree and leave every device
# clean: this exercises open/close, state.db transitions and the teardown path.
print "== repeated mount / unmount cycles\n";
for ( my $i = 0 ; $i < 5 ; $i++ ) {
    mount_array() or exit 1;
    fail("cycle $i content") unless ( slurp("$mp/healme") // "" ) eq "good\n";
    fail("cycle $i post")    unless ( slurp("$mp/post") // "" ) eq "post-remount\n";
    unmount_array();
}

print "== audit is read-only on persistent state\n";
{
    my @files = qw(muxfs.conf state.db meta.db assign.db);
    my %before;
    for my $d ( $dev_a, $dev_b ) {
        my $db = slurp("$d/.muxfs/state.db");
        my @f  = defined($db) && length($db) == 40 ? unpack( "Q<5", $db ) : ();
        print "diag: pre-audit $d seq=$f[0] mounted=$f[1] working=$f[2] "
          . "restoring=$f[3] degraded=$f[4]\n";
    }
    my ( $aout, $ast ) =
      capture( "/bin/sh", "-c", "$EMUXFS audit $dev_a $dev_b 2>&1" );
    print "diag: audit out: @$aout (status " . ( $ast >> 8 ) . ")\n";
    for my $d ( $dev_a, $dev_b ) {
        for my $f (@files) {
            $before{"$d/$f"} = slurp("$d/.muxfs/$f");
        }
    }
    must_run( "audit (clean)", $EMUXFS, "audit", $dev_a, $dev_b );
    for my $d ( $dev_a, $dev_b ) {
        for my $f (@files) {
            my $after = slurp("$d/.muxfs/$f");
            fail("audit modified $f on $d")
              unless defined( $before{"$d/$f"} )
              && defined($after)
              && $before{"$d/$f"} eq $after;
        }
    }
}

print "== audit detects corruption\n";
open( my $cfh, ">>", "$dev_a/post" ) or fail("corrupt post: $!");
print $cfh "corrupt\n";
close($cfh);
my ( $audit_lines, $audit_st ) = capture( $EMUXFS, "audit", $dev_a, $dev_b );
if ( $audit_st != 0 ) {
    fail( "audit exited " . ( $audit_st >> 8 ) );
}
else {
    my $expected = "$dev_a/post";
    my $found    = grep { $_ eq $expected } @$audit_lines;
    fail("audit did not report $expected") unless $found;
}

print "== heal repairs corruption\n";
must_run( "heal", $EMUXFS, "heal", $dev_a, $dev_b );
{
    for my $d ( $dev_a, $dev_b ) {
        my $db = slurp("$d/.muxfs/state.db");
        if ( defined($db) && length($db) == 40 ) {
            my @f = unpack( "Q<5", $db );
            print "diag: $d seq=$f[0] mounted=$f[1] working=$f[2] "
              . "restoring=$f[3] degraded=$f[4]\n";
        }
        else {
            print "diag: $d state.db unreadable\n";
        }
    }
}
fail("heal did not converge") if compare( "$dev_a/post", "$dev_b/post" ) != 0;
must_run( "audit after heal", $EMUXFS, "audit", $dev_a, $dev_b );

print "== sync to a replacement mirror\n";
must_run( "sync", $EMUXFS, "sync", $dev_c, $dev_a, $dev_b );
fail("sync c != a") if compare( "$dev_c/post", "$dev_a/post" ) != 0;
fail("sync c != b") if compare( "$dev_c/post", "$dev_b/post" ) != 0;

print "== hard links are refused\n";
mount_array() or exit 1;

# Creating a hard link through the filesystem must fail: emuxfs cannot
# represent two names for one inode.
if ( system( "ln", "$mp/post", "$mp/hard" ) == 0 ) {
    fail("hard link unexpectedly created through emuxfs");
    system( "rm", "-f", "$mp/hard" );
}
unmount_array();

# A hard link introduced directly into a mirror must be detected by audit.
must_run( "make direct hard link", "ln", "$dev_a/post", "$dev_a/post-hard" );
{
    my ( $out, $st ) = capture( $EMUXFS, "audit", $dev_a, $dev_b );
    fail("audit accepted a hard-linked device") if $st == 0;
}
system( "rm", "-f", "$dev_a/post-hard" );
must_run( "audit after hard link removal", $EMUXFS, "audit", $dev_a, $dev_b );

print "== version\n";
must_run( "version", $EMUXFS, "version" );

if ( $failures != 0 ) {
    print STDERR "$failures integration test(s) failed\n";
    exit 1;
}
print "All integration tests passed\n";
exit 0;
