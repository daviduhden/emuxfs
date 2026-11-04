#!/usr/bin/env perl
# Optional workload measurements on an already mounted disposable emuxfs array.
# Creates and removes only its own private temporary directory. See TESTING.md.
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Temp qw(tempdir);
use File::Path qw(remove_tree);
use Time::HiRes qw(time);

my ( $mount, $mirrors, $pid ) = @ARGV;
die "usage: perl tests/benchmark.pl mountpoint mirror_count [daemon_pid]\n"
    unless defined($mount) && defined($mirrors) && $mirrors =~ /\A[1-9][0-9]*\z/x
    && ( !defined($pid) || $pid =~ /\A[1-9][0-9]*\z/x );
$mount = abs_path($mount);
die "mountpoint must be an existing directory\n" unless defined($mount) && -d $mount;
$mount =~ s{/+$}{}x unless $mount eq "/";
my $sandbox = tempdir( 'emuxfs-bench.XXXXXX', DIR => $mount, CLEANUP => 0 );
my $owner = $$;
my $prefix = $mount eq "/" ? "" : $mount;
END {
    if ( defined($sandbox) && $$ == $owner ) {
        die "unsafe benchmark cleanup\n"
            unless $sandbox =~ m{\A\Q$prefix\E/emuxfs-bench\.[^/]+\z}x;
        remove_tree($sandbox, { safe => 1 });
    }
}
local $SIG{INT} = local $SIG{TERM} = sub { exit 130; };
my $max_rss = 0;
sub sample_rss {
    return unless defined $pid;
    open my $ps, '-|', 'ps', '-o', 'rss=', '-p', $pid or die "ps: $!\n";
    my $rss = <$ps>;
    close $ps or die "cannot sample daemon $pid\n";
    die "daemon RSS unavailable\n" unless defined($rss) && $rss =~ /\A\s*([0-9]+)\s*\z/x;
    $max_rss = $1 if $1 > $max_rss;
    return;
}
sub measure {
    my ( $name, $bytes, $iterations, $operation ) = @_;
    my @elapsed;
    for ( 1 .. $iterations ) {
        my $start = time();
        $operation->($_);
        push @elapsed, time() - $start;
        sample_rss();
    }
    @elapsed = sort { $a <=> $b } @elapsed;
    my $total = 0;
    $total += $_ for @elapsed;
    printf "%s,%d,%d,%d,%.9f,%.9f,%.3f,%s\n", $name, $mirrors, $bytes,
        $iterations, $elapsed[int(@elapsed / 2)], $elapsed[-1],
        $total > 0 ? $bytes * $iterations / $total : 0,
        defined($pid) ? $max_rss : 'unmeasured';
    return;
}
print "workload,mirrors,bytes_per_op,iterations,median_seconds,max_seconds,bytes_per_second,max_sampled_daemon_rss_kib\n";
for my $size ( 0, 1, 4095, 4096, 4097, 65536, 1048576 ) {
    my $data = 'x' x $size;
    measure("create_$size", $size, 10, sub {
        my ($n) = @_;
        open my $file, '>', "$sandbox/file-$size-$n" or die "create: $!\n";
        print {$file} $data or die "write: $!\n";
        close $file or die "close: $!\n";
    });
    measure("read_$size", $size, 10, sub {
        my ($n) = @_;
        open my $file, '<', "$sandbox/file-$size-$n" or die "open: $!\n";
        local $/ = undef;
        my $actual = <$file> // '';
        die "content mismatch\n" unless $actual eq $data;
        close $file or die "close: $!\n";
    });
}
my $deep = $sandbox;
measure('mkdir_depth_64', 0, 64, sub {
    $deep .= '/d';
    mkdir $deep or die "mkdir: $!\n";
});
mkdir "$sandbox/wide" or die "mkdir: $!\n";
measure('create_width_256', 1, 256, sub {
    my ($n) = @_;
    open my $file, '>', "$sandbox/wide/$n" or die "create: $!\n";
    print {$file} 'x' or die "write: $!\n";
    close $file or die "close: $!\n";
});
measure('readdir_width_256', 0, 10, sub {
    opendir my $dir, "$sandbox/wide" or die "opendir: $!\n";
    my @names = readdir $dir;
    closedir $dir or die "closedir: $!\n";
    die "missing entries\n" unless @names == 258;
});
