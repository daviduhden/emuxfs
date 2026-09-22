#!/usr/bin/env perl
#
# Parallel-load stress test for emuxfs.
#
# Formats and mounts a two-device array, then runs 32 concurrent client
# processes that each churn through mkdir/create/write/read/rename/unlink/rmdir
# on names of their own, leaving one deterministic file behind.  When they
# finish the array is unmounted and the test requires that audit is clean and
# that the two mirrors are byte-for-byte identical, including the artifacts.
#
# The FUSE daemon is single-threaded (fuse_loop(3)), so the requests are
# serialized there; the point of the test is the concurrent arrival path
# through the kernel and the integrity of the committed state afterwards.
#
# Everything happens inside a private temporary sandbox.  Requires root and
# /dev/fuse0; see TESTING.md.

use strict;
use warnings;

use Cwd           qw(getcwd);
use File::Path    qw(make_path remove_tree);
use File::Temp    qw(tempdir);
use POSIX         qw(WNOHANG);

my $EMUXFS  = $ENV{EMUXFS}           // ( getcwd() . "/emuxfs" );
my $WORKERS = $ENV{PARALLEL_WORKERS} // 32;
my $ITERS   = $ENV{PARALLEL_ITERS}   // 20;
my $TIMEOUT = $ENV{PARALLEL_TIMEOUT} // 900;

unless ( -x $EMUXFS ) {
    print STDERR "emuxfs binary not found at $EMUXFS (set EMUXFS=...)\n";
    exit 2;
}

if ( $> != 0 ) {
    print STDERR "SKIP: the parallel test requires root\n";
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

$| = 1;

my $tmpbase = $ENV{TMPDIR} // "/tmp";
$tmpbase =~ s{/+$}{};

my $sandbox = tempdir( "emuxfs-parallel.XXXXXX", DIR => $tmpbase, CLEANUP => 0 );
unless ( $sandbox =~ m{\A\Q$tmpbase\E/[^/]+\z} ) {
    print STDERR "Refusing to run: sandbox '$sandbox' is outside '$tmpbase'\n";
    exit 2;
}

my $dev_a    = "$sandbox/dev_a";
my $dev_b    = "$sandbox/dev_b";
my $mp       = "$sandbox/mp";
my $mlog     = "$sandbox/mount-fg.log";
my $mounted  = 0;
my $failures = 0;

my $parent = $$;

sub fail {
    my ($msg) = @_;
    print STDERR "FAIL: $msg\n";
    $failures++;
}

sub slurp {
    my ($path) = @_;
    open( my $fh, "<", $path ) or return undef;
    local $/;
    my $data = <$fh>;
    close($fh);
    return $data;
}

sub run {
    system(@_);
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

my $cleaned = 0;

sub cleanup {
    return if $$ != $parent;    # Never run the parent's cleanup in a worker.
    return if $cleaned++;
    if ($mounted) {
        system("umount $mp >/dev/null 2>&1");
        system("umount -f $mp >/dev/null 2>&1");
        $mounted = 0;
    }
    if ( $sandbox =~ m{\A\Q$tmpbase\E/[^/]+\z} ) {
        remove_tree($sandbox);
    }
}
END { cleanup() }
$SIG{INT} = $SIG{TERM} = sub { cleanup(); exit 130 };

sub state_fields {
    my ($dev) = @_;
    my $data = slurp("$dev/.muxfs/state.db");
    return undef unless defined($data) && length($data) == 5 * 8;
    return [ unpack( "Q<5", $data ) ];
}

sub wait_state_clean {
    my ($dev) = @_;
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $st = state_fields($dev);
        return 1
          if defined($st) && $st->[1] == 0 && $st->[2] == 0 && $st->[3] == 0;
        select( undef, undef, undef, 0.02 );
    }
    return 0;
}

sub mount_array {
    unlink($mlog);
    system("EMUXFS_TRACE=1 $EMUXFS mount -f $mp $dev_a $dev_b >'$mlog' 2>&1 &");
    $mounted = 1;
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $l = slurp($mlog);
        last if defined($l) && $l =~ /entering fuse_loop/;
        select( undef, undef, undef, 0.02 );
    }
}

# One worker: churn through its own private names, then leave a marker file.
sub worker {
    my ($id) = @_;
    my $tag = sprintf( "w%02d", $id );

    for my $i ( 1 .. $ITERS ) {
        my $dir = "$mp/$tag-d$i";
        return 1 unless mkdir($dir);

        my $name = "$dir/f";
        open( my $fh, ">", $name ) or return 1;
        print $fh "$tag:$i\n" or return 1;
        close($fh) or return 1;

        my $got = slurp($name);
        return 1 unless defined($got) && $got eq "$tag:$i\n";

        my $name2 = "$dir/f.ren";
        return 1 unless rename( $name, $name2 );

        $got = slurp($name2);
        return 1 unless defined($got) && $got eq "$tag:$i\n";

        return 1 unless unlink($name2);
        return 1 unless rmdir($dir);
    }

    open( my $fh, ">", "$mp/final_$tag" ) or return 1;
    print $fh "$tag:done\n" or return 1;
    close($fh) or return 1;

    return 0;
}

# A comparable description of a device tree, excluding the .muxfs directory.
sub manifest {
    my ($root) = @_;
    my %m;
    my @stack = ("");

    while (@stack) {
        my $rel = pop @stack;
        my $dir = $rel eq "" ? $root : "$root/$rel";
        opendir( my $dh, $dir ) or return undef;
        my @ent = readdir($dh);
        closedir($dh);
        for my $e (@ent) {
            next if $e eq "." || $e eq "..";
            my $crel = $rel eq "" ? $e : "$rel/$e";
            next if $crel eq ".muxfs";
            my @st = lstat("$root/$crel");
            next unless @st;
            my $mode = $st[2] & 07777;
            if ( -l _ ) {
                my $t = readlink("$root/$crel");
                $m{$crel} = "l:" . ( defined($t) ? $t : "" );
            }
            elsif ( -d _ ) {
                $m{$crel} = sprintf( "d:%04o", $mode );
                push @stack, $crel;
            }
            elsif ( -f _ ) {
                my $data = slurp("$root/$crel");
                $m{$crel} =
                  sprintf( "f:%04o:%d:", $mode, $st[7] )
                  . ( defined($data) ? $data : "<unreadable>" );
            }
            else {
                $m{$crel} = sprintf( "?:%04o", $mode );
            }
        }
    }

    return \%m;
}

# ---------------------------------------------------------------------------

make_path( $dev_a, $dev_b, $mp );

print "== format\n";
must_run( "format", $EMUXFS, "format", "-a", "sha1", $dev_a, $dev_b )
  or exit 1;

print "== mount\n";
mount_array();

print "== $WORKERS workers x $ITERS iterations\n";
my @pids;
for my $id ( 0 .. $WORKERS - 1 ) {
    my $pid = fork();
    if ( !defined $pid ) {
        fail("fork: $!");
        last;
    }
    if ( $pid == 0 ) {
        my $rc = worker($id);
        POSIX::_exit( $rc ? 1 : 0 );
    }
    push @pids, $pid;
}

my %status;
my %alive = map { $_ => 1 } @pids;
my $deadline = time() + $TIMEOUT;
my $last     = time();
while (%alive) {
    for my $pid ( sort { $a <=> $b } keys %alive ) {
        my $r = waitpid( $pid, WNOHANG );
        if ( $r == $pid ) {
            $status{$pid} = $?;
            delete $alive{$pid};
        }
    }
    last unless %alive;
    if ( time() > $deadline ) {
        fail("$TIMEOUT-second timeout with " . scalar(keys %alive)
              . " worker(s) still running");
        kill( "KILL", keys %alive );
        waitpid( $_, 0 ) for keys %alive;
        %alive = ();
        last;
    }
    if ( time() >= $last + 5 ) {
        $last = time();
        my $done = scalar(@pids) - scalar(keys %alive);
        print "  $done/" . scalar(@pids) . " workers finished\n";
    }
    select( undef, undef, undef, 0.05 );
}

my $bad = 0;
for my $st ( values %status ) {
    $bad++ if $st != 0;
}
fail("$bad worker(s) failed") if $bad;

print "== unmount and verify\n";
must_run( "umount", "umount", $mp );
$mounted = 0;
wait_state_clean($dev_a) or fail("dev_a not clean after umount");
wait_state_clean($dev_b) or fail("dev_b not clean after umount");

must_run( "audit", $EMUXFS, "audit", $dev_a, $dev_b );

for my $id ( 0 .. $WORKERS - 1 ) {
    my $tag = sprintf( "w%02d", $id );
    for my $dev ( $dev_a, $dev_b, $mp ) {
        my $got = slurp("$dev/final_$tag");
        fail("final_$tag missing or wrong on $dev")
          unless defined($got) && $got eq "$tag:done\n";
    }
}

my $ma = manifest($dev_a);
my $mb = manifest($dev_b);
if ( !defined($ma) || !defined($mb) ) {
    fail("cannot build a device manifest");
}
else {
    for my $k ( sort keys %$ma ) {
        if ( !exists $mb->{$k} ) {
            fail("only on dev_a: $k");
            next;
        }
        fail("mirrors differ: $k") if $ma->{$k} ne $mb->{$k};
    }
    for my $k ( sort keys %$mb ) {
        fail("only on dev_b: $k") unless exists $ma->{$k};
    }
}

if ( $failures != 0 ) {
    print STDERR "$failures parallel test(s) failed\n";
    exit 1;
}
print "All parallel tests passed\n";
exit 0;
