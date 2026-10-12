#!/usr/bin/env perl
#
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
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

use Cwd         qw(getcwd);
use File::Path  qw(make_path remove_tree);
use File::Temp  qw(tempdir);
use POSIX       qw(WNOHANG);
use Time::HiRes ();

my $EMUXFS  = $ENV{EMUXFS}           // ( getcwd() . "/emuxfs" );
my $WORKERS = $ENV{PARALLEL_WORKERS} // 32;
my $ITERS   = $ENV{PARALLEL_ITERS}   // 20;
my $TIMEOUT = $ENV{PARALLEL_TIMEOUT} // 900;

if ( !-x $EMUXFS ) {
    print STDERR "emuxfs binary not found at $EMUXFS (set EMUXFS=...)\n";
    exit 2;
}

if ( $> != 0 ) {
    print STDERR "SKIP: the parallel test requires root\n";
    exit 0;
}

# Create the FUSE device node if the image did not populate /dev.
if ( !-c "/dev/fuse0" ) {
    system( "sh", "-c", "cd /dev && MAKEDEV fuse >/dev/null 2>&1" );
}
if ( !-c "/dev/fuse0" ) {
    print STDERR "SKIP: /dev/fuse0 is not available\n";
    exit 0;
}

local $| = 1;

my $tmpbase = $ENV{TMPDIR} // "/tmp";
$tmpbase =~ s{/+$}{}x;

my $sandbox =
  tempdir( "emuxfs-parallel.XXXXXX", DIR => $tmpbase, CLEANUP => 0 );
if ( $sandbox !~ m{\A\Q$tmpbase\E/[^/]+\z}x ) {
    print STDERR "Refusing to run: sandbox '$sandbox' is outside '$tmpbase'\n";
    exit 2;
}

my $dev_a    = "$sandbox/dev_a";
my $dev_b    = "$sandbox/dev_b";
my $mp       = "$sandbox/mp";
my $mlog     = "$sandbox/mount-fg.log";
my $mounted  = 0;
my $failures = 0;
my $daemon_pid;

my $parent = $$;

sub fail {
    my ($msg) = @_;
    print STDERR "FAIL: $msg\n";
    $failures++;
    return;
}

sub slurp {
    my ($path) = @_;
    open( my $fh, "<", $path ) or return;
    local $/ = undef;
    my $data = <$fh>;
    close($fh);
    return $data;
}

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

my $cleaned = 0;

sub cleanup {
    return if $$ != $parent;    # Never run the parent's cleanup in a worker.
    return if $cleaned++;
    if ($mounted) {
        system("umount $mp >/dev/null 2>&1");
        system("umount -f $mp >/dev/null 2>&1");
        $mounted = 0;
    }
    if ( defined $daemon_pid ) {
        kill( "TERM", $daemon_pid );
        for ( my $i = 0 ; $i < 50 ; $i++ ) {
            last if waitpid( $daemon_pid, WNOHANG ) == $daemon_pid;
            Time::HiRes::sleep(0.02);
        }
        kill( "KILL", $daemon_pid );
        waitpid( $daemon_pid, 0 );
        $daemon_pid = undef;
    }
    if ( $sandbox =~ m{\A\Q$tmpbase\E/[^/]+\z}x ) {
        remove_tree($sandbox);
    }
    return;
}
END { cleanup() }
local $SIG{INT} = local $SIG{TERM} = sub { cleanup(); exit 130 };

sub state_fields {
    my ($dev) = @_;
    my $data = slurp("$dev/.muxfs/state.db");
    return unless defined($data) && length($data) == 5 * 8;
    return [ unpack( "Q<5", $data ) ];
}

sub wait_state_clean {
    my ($dev) = @_;
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $st = state_fields($dev);
        return 1
          if defined($st) && $st->[1] == 0 && $st->[2] == 0 && $st->[3] == 0;
        Time::HiRes::sleep(0.02);
    }
    return 0;
}

sub mount_array {
    unlink($mlog);

    # Run the daemon as a direct child so that its exit status (including a
    # fatal signal) can be reported if it dies while serving requests.
    my $pid = fork();
    if ( !defined $pid ) {
        fail("fork (mount): $!");
        return 0;
    }
    if ( $pid == 0 ) {
        open( STDOUT, ">",  $mlog )    or POSIX::_exit(127);
        open( STDERR, ">&", \*STDOUT ) or POSIX::_exit(127);
        local $ENV{EMUXFS_TRACE} = 1;
        exec( $EMUXFS, "mount", "-f", $mp, $dev_a, $dev_b )
          or POSIX::_exit(127);
    }
    $daemon_pid = $pid;
    $mounted    = 1;

    my $ready = 0;
    for ( my $i = 0 ; $i < 500 ; $i++ ) {
        my $l = slurp($mlog);
        if ( defined($l) && $l =~ /entering fuse_loop/ ) {
            $ready = 1;
            last;
        }
        Time::HiRes::sleep(0.02);
    }
    fail("mount did not become ready") unless $ready;
    return $ready;
}

# Report (once) whether the daemon child has already exited or been killed.
sub report_daemon_death {
    return 1 unless defined $daemon_pid;
    my $r = waitpid( $daemon_pid, WNOHANG );
    return 1 if $r != $daemon_pid;
    my $st = $?;
    if ( $st & 127 ) {
        fail( "daemon died from signal " . ( $st & 127 ) );
    }
    elsif ( ( $st >> 8 ) != 0 ) {
        fail( "daemon exited with status " . ( $st >> 8 ) );
    }
    $daemon_pid = undef;
    return 0;
}

# Reap the daemon without reporting (used after a clean unmount or cleanup).
sub reap_daemon {
    return unless defined $daemon_pid;
    waitpid( $daemon_pid, 0 );
    $daemon_pid = undef;
    return;
}

# Unmount with a bound so that a wedged mount cannot hang the whole test.
sub umount_array {
    my $ok = eval {
        local $SIG{ALRM} = sub { die "umount timeout\n" };
        alarm(30);
        system("umount $mp >/dev/null 2>&1");
        alarm(0);
        1;
    };
    alarm(0);
    if ( !$ok ) {
        fail("umount did not complete within 30s");
        system("umount -f $mp >/dev/null 2>&1");
    }
    $mounted = 0;
    return $ok;
}

# Confirm that the mount is actually serving requests before loading it.
sub probe_mount {
    my $tag = "probe.$$";
    my $dir = "$mp/$tag";
    if ( !mkdir($dir) ) {
        fail("mount probe mkdir failed: $!");
        return 0;
    }
    my $pf;
    if ( !open( $pf, ">", "$dir/p" ) ) {
        fail("mount probe open failed: $!");
        rmdir($dir);
        return 0;
    }
    print $pf "probe\n" or fail("mount probe write failed: $!");
    close($pf)          or fail("mount probe close failed: $!");
    my $got = slurp("$dev_a/$tag/p");
    fail("mount probe was not mirrored to dev_a")
      unless defined($got) && $got eq "probe\n";
    unlink("$mp/$tag/p") or fail("mount probe unlink failed: $!");
    rmdir($dir)          or fail("mount probe rmdir failed: $!");
    return 1;
}

# One worker: churn through its own private names, then leave a marker file.
# A failure is reported through a per-worker file so that the parent can name
# the exact operation that failed.
sub worker {
    my ($id) = @_;
    my $tag = sprintf( "w%02d", $id );
    my $err;

    eval {
        for my $i ( 1 .. $ITERS ) {
            churn_iteration( $tag, $i );
        }

        write_marker($tag);

        1;
    } or $err = ( $@ // "unknown failure\n" );

    if ( defined $err ) {
        $err =~ s/\s+\z//x;
        if ( open( my $eh, ">", "$sandbox/worker-$tag.err" ) ) {
            print $eh "$err\n";
            close($eh);
        }
        return 1;
    }

    return 0;
}

# One churn step for worker(): create, write, read, rename, read, unlink, rmdir.
sub churn_iteration {
    my ( $tag, $i ) = @_;
    my $dir = "$mp/$tag-d$i";
    die "mkdir $dir: $!\n" unless mkdir($dir);

    my $name = "$dir/f";
    open( my $fh, ">", $name )
      or die "open $name: $!\n";
    print $fh "$tag:$i\n"
      or die "write $name: $!\n";
    close($fh)
      or die "close $name: $!\n";

    my $got = slurp($name);
    die "read $name: wrong or missing content\n"
      unless defined($got) && $got eq "$tag:$i\n";

    my $name2 = "$dir/f.ren";
    die "rename $name -> $name2: $!\n"
      unless rename( $name, $name2 );

    $got = slurp($name2);
    die "read $name2: wrong or missing content\n"
      unless defined($got) && $got eq "$tag:$i\n";

    die "unlink $name2: $!\n" unless unlink($name2);
    die "rmdir $dir: $!\n"    unless rmdir($dir);

    # Best-effort progress for the parent (sandbox, not the mount).
    if ( open( my $pf, ">", "$sandbox/progress/$tag" ) ) {
        print $pf "$i\n";
        close($pf);
    }
    return;
}

# Write worker()'s deterministic marker file and final progress record.
sub write_marker {
    my ($tag) = @_;
    open( my $fh, ">", "$mp/final_$tag" )
      or die "open final_$tag: $!\n";
    print $fh "$tag:done\n"
      or die "write final_$tag: $!\n";
    close($fh)
      or die "close final_$tag: $!\n";

    if ( open( my $pf, ">", "$sandbox/progress/$tag" ) ) {
        print $pf "done\n";
        close($pf);
    }
    return;
}

# Read the per-worker progress markers written by worker().  The daemon is
# single-threaded, so all workers advance in lockstep and the number of
# *finished workers* stays at zero until the very end; counting completed
# iterations gives useful progress instead.
sub progress_line {
    my $done  = 0;
    my $iters = 0;
    for my $id ( 0 .. $WORKERS - 1 ) {
        my $tag = sprintf( "w%02d", $id );
        my $v   = slurp("$sandbox/progress/$tag");
        next unless defined $v;
        chomp $v;
        if ( $v eq "done" ) {
            ++$done;
            $iters += $ITERS;
        }
        elsif ( $v =~ /\A[0-9]+\z/x ) {
            $iters += $v;
        }
    }
    my $total = $WORKERS * $ITERS;
    return "  $done/$WORKERS workers, $iters/$total iterations";
}

# A comparable description of a device tree, excluding the .muxfs directory.
sub manifest {
    my ($root) = @_;
    my %m;
    my @stack = (q{});

    while (@stack) {
        my $rel = pop @stack;
        my $dir = $rel eq q{} ? $root : "$root/$rel";
        opendir( my $dh, $dir ) or return;
        my @ent = readdir($dh);
        closedir($dh);
        for my $e (@ent) {
            next if $e eq q{.} || $e eq q{..};
            my $crel = $rel eq q{} ? $e : "$rel/$e";
            next if $crel eq ".muxfs";
            my @st = lstat("$root/$crel");
            next unless @st;
            my $mode = $st[2] & oct(7777);
            if ( -l _ ) {
                my $t = readlink("$root/$crel");
                $m{$crel} = "l:" . ( defined($t) ? $t : q{} );
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

# Format both devices, mount the array and probe it.
sub setup_devices {
    make_path( $dev_a, $dev_b, $mp );
    make_path("$sandbox/progress");

    print "== format\n";
    must_run( "format", $EMUXFS, "format", "-a", "sha1", $dev_a, $dev_b )
      or exit 1;

    print "== mount\n";
    mount_array();
    probe_mount();
    return;
}

# Fork one worker per id and return the list of child pids.
sub spawn_workers {
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
    return @pids;
}

# Reap the workers, reporting progress, and return pid => status.
sub wait_for_workers {
    my (@pids) = @_;
    my %status;
    my %alive       = map { $_ => 1 } @pids;
    my $deadline    = time() + $TIMEOUT;
    my $last_report = time();
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
            fail(   "$TIMEOUT-second timeout with "
                  . scalar( keys %alive )
                  . " worker(s) still running" );
            kill( "KILL", keys %alive );
            waitpid( $_, 0 ) for keys %alive;
            %alive = ();
            last;
        }
        if ( time() >= $last_report + 2 ) {
            $last_report = time();
            print progress_line(), "\n";
        }
        Time::HiRes::sleep(0.05);
    }
    print progress_line(), "\n";
    return %status;
}

# Report each failed worker and dump its per-worker error file.
sub check_workers {
    my (%status) = @_;
    my $bad = 0;
    for my $st ( values %status ) {
        $bad++ if $st != 0;
    }
    if ($bad) {
        fail("$bad worker(s) failed");
        for my $id ( 0 .. $WORKERS - 1 ) {
            my $tag = sprintf( "w%02d", $id );
            my $e   = slurp("$sandbox/worker-$tag.err");
            print STDERR "worker $tag: $e\n" if defined $e;
        }
    }
    return;
}

# If the daemon is already gone here, say how it went; also flag degraded
# devices, since a device marked degraded during the load explains the EIO
# storm.
sub check_daemon_and_devices {
    report_daemon_death();

    for my $dev ( $dev_a, $dev_b ) {
        my $st = state_fields($dev);
        if ( !defined $st ) {
            fail("cannot read state.db for $dev after the load");
        }
        elsif ( $st->[4] != 0 ) {
            fail("$dev is degraded after the load");
        }
    }
    return;
}

# The mount point is empty again after unmount, so check it first.
sub verify_through_mount {
    print "== verify through the mount\n";
    for my $id ( 0 .. $WORKERS - 1 ) {
        my $tag = sprintf( "w%02d", $id );
        my $got = slurp("$mp/final_$tag");
        fail("final_$tag missing or wrong on the mount")
          unless defined($got) && $got eq "$tag:done\n";
    }
    return;
}

# Unmount, audit, and confirm the final marker on both mirrors.
sub unmount_and_audit {
    print "== unmount and verify the mirrors\n";
    umount_array();
    reap_daemon();
    wait_state_clean($dev_a) or fail("dev_a not clean after umount");
    wait_state_clean($dev_b) or fail("dev_b not clean after umount");

    must_run( "audit", $EMUXFS, "audit", $dev_a, $dev_b );

    for my $id ( 0 .. $WORKERS - 1 ) {
        my $tag = sprintf( "w%02d", $id );
        for my $dev ( $dev_a, $dev_b ) {
            my $got = slurp("$dev/final_$tag");
            fail("final_$tag missing or wrong on $dev")
              unless defined($got) && $got eq "$tag:done\n";
        }
    }
    return;
}

# Compare the two device trees entry by entry.
sub compare_mirrors {
    my $ma = manifest($dev_a);
    my $mb = manifest($dev_b);
    if ( !defined($ma) || !defined($mb) ) {
        fail("cannot build a device manifest");
    }
    else {
        for my $k ( sort keys %{$ma} ) {
            if ( !exists $mb->{$k} ) {
                fail("only on dev_a: $k");
                next;
            }
            fail("mirrors differ: $k") if $ma->{$k} ne $mb->{$k};
        }
        for my $k ( sort keys %{$mb} ) {
            fail("only on dev_b: $k") unless exists $ma->{$k};
        }
    }
    return;
}

# Dump the daemon log diagnostics that explain the first failure.
sub print_diagnostics {
    my $l = slurp($mlog);
    if ( defined $l ) {
        my @all = split( /\n/x, $l );
        my @key = grep { /PHANTOM|:\ fail|stage=|readback:/x } @all;
        @key = @key[ 0 .. 79 ] if @key > 80;
        if (@key) {
            print STDERR "diag: daemon failure lines:\n", join( "\n", @key ),
              "\n";
        }
        my $first = -1;
        for my $i ( 0 .. $#all ) {
            if ( $all[$i] =~ /fail|readback:|dir_meta_recompute:|degraded=1/x )
            {
                $first = $i;
                last;
            }
        }
        if ( $first >= 0 ) {
            my $s = $first - 30;
            $s = 0 if $s < 0;
            my $e = $first + 30;
            $e = $#all if $e > $#all;
            print STDERR "diag: daemon lines around the first failure:\n",
              join( "\n", @all[ $s .. $e ] ), "\n";
        }
        my @t = @all > 120 ? @all[ -120 .. -1 ] : @all;
        print STDERR "diag: daemon trace tail:\n", join( "\n", @t ), "\n";
    }
    return;
}

# ---------------------------------------------------------------------------

setup_devices();
my @pids   = spawn_workers();
my %status = wait_for_workers(@pids);
check_workers(%status);
check_daemon_and_devices();
verify_through_mount();
unmount_and_audit();
compare_mirrors();

if ( $failures != 0 ) {
    print_diagnostics();
    print STDERR "$failures parallel test(s) failed\n";
    exit 1;
}
print "All parallel tests passed\n";
exit 0;
