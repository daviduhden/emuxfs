#!/usr/bin/env perl
#
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
#
# Legacy end-to-end test suite for emuxfs, ported from the original shell
# script to Perl.
#
# Unlike tests/integration/integration.pl this suite uses the paths supplied by
# test.conf rather than a private sandbox, so those paths must be dedicated
# scratch directories.  The suite refuses to run unless test.conf exists and
# the configured paths look safe.
#
# See TESTING.md.

use strict;
use warnings;

use Cwd            qw(getcwd abs_path);
use File::Compare  qw(compare);
use File::Copy     qw(copy);
use File::Path     qw(make_path remove_tree);
use File::Basename qw(dirname);

my $EMUXFS = $ENV{EMUXFS} // "./emuxfs";

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

unless ( -e "test.conf" ) {
    print STDERR "Please read test.conf.dist.\n";
    exit 1;
}

# Parse test.conf in Perl: 'key = value' lines, '#' comments, optional quotes.
my %conf;
open( my $cfh, "<", "test.conf" ) or die "test.conf: $!\n";
while ( my $line = <$cfh> ) {
    chomp $line;
    $line                        =~ s/\r\z//;    # tolerate CRLF
    $line                        =~ s/\A\s+//;
    $line                        =~ s/\s+\z//;
    next if $line eq "" || $line =~ /\A#/;
    my ( $key, $val ) = split( /\s*=\s*/, $line, 2 );
    next unless defined $key && defined $val;
    $val =~ s/\A(['"])(.*)\1\z/$2/;              # strip matching quotes
    $conf{$key} = $val;
}
close($cfh);

my $mp          = $conf{mp}          // "";
my $dev_a       = $conf{dev_a}       // "";
my $dev_b       = $conf{dev_b}       // "";
my $dev_c       = $conf{dev_c}       // "";
my $test_tmp    = $conf{test_tmp}    // "";
my $unpriv_user = $conf{unpriv_user} // "";

# Refuse to operate on unsafe or ambiguous paths.
sub safe_path {
    my ( $name, $path ) = @_;
    return "$name is not set" if $path eq "";
    return "$name is not absolute: $path" unless $path =~ m{\A/};
    return "$name is the root directory" if $path eq "/";
    return "$name contains '..': $path"  if $path =~ m{(^|/)\.\.(/|\z)};
    return undef;
}
foreach my $pair (
    [ mp       => $mp ],
    [ dev_a    => $dev_a ],
    [ dev_b    => $dev_b ],
    [ dev_c    => $dev_c ],
    [ test_tmp => $test_tmp ]
  )
{
    my ( $name, $path ) = @$pair;
    my $err = safe_path( $name, $path );
    die "$err\n" if defined $err;
}
my %seen;
foreach my $path ( $mp, $dev_a, $dev_b, $dev_c, $test_tmp ) {
    die "duplicate path in test.conf: $path\n" if $seen{$path}++;
}

die "emuxfs binary not found at $EMUXFS (set EMUXFS=...)\n"
  unless -x $EMUXFS;

my $chk_alg = "invalid";
my $mounted = 0;
my $started = 0;

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

sub quiet {
    my (@cmd) = @_;
    open( my $fh, "-|", @cmd ) or die "cannot run @cmd: $!\n";
    while ( defined( my $line = <$fh> ) ) { }
    close($fh);
    return $? >> 8;
}

sub must_quiet {
    my ( $what, @cmd ) = @_;
    my $st = quiet(@cmd);
    die "$what failed: @cmd\n" if $st != 0;
}

sub must_fail {
    my ( $what, @cmd ) = @_;
    my $st = quiet(@cmd);
    die "$what unexpectedly succeeded: @cmd\n" if $st == 0;
}

sub capture {
    my (@cmd) = @_;
    open( my $fh, "-|", @cmd ) or die "cannot run @cmd: $!\n";
    my @lines = <$fh>;
    close($fh);
    my $st = $? >> 8;
    chomp @lines;
    return ( \@lines, $st );
}

sub slurp {
    my ($path) = @_;
    open( my $fh, "<", $path ) or die "$path: $!\n";
    local $/;
    my $data = <$fh>;
    close($fh);
    return $data;
}

sub spit {
    my ( $path, $data ) = @_;
    open( my $fh, ">", $path ) or die "$path: $!\n";
    print $fh $data;
    close($fh);
}

sub append_to {
    my ( $path, $data ) = @_;
    open( my $fh, ">>", $path ) or die "$path: $!\n";
    print $fh $data;
    close($fh);
}

# Write $bytes of random data without depending on dd(1).
sub make_random_file {
    my ( $path, $bytes ) = @_;
    open( my $in,  "<", "/dev/urandom" ) or die "/dev/urandom: $!\n";
    open( my $out, ">", $path )          or die "$path: $!\n";
    my $left = $bytes;
    while ( $left > 0 ) {
        my $n = $left > 65536 ? 65536 : $left;
        my $buf;
        my $got = read( $in, $buf, $n );
        die "short read from /dev/urandom\n" unless defined $got && $got > 0;
        print $out $buf;
        $left -= $got;
    }
    close($out);
    close($in);
}

sub must_emuxfs {
    my (@args) = @_;
    my @cmd    = ( $EMUXFS, @args );
    my $st     = quiet(@cmd);
    die "emuxfs @args failed (exit $st)\n" if $st != 0;
}

sub sleep_tick {
    select( undef, undef, undef, 0.01 );
}

sub list_dir {
    my ($dir) = @_;
    my ( $lines, $st ) = capture( "ls", $dir );
    return "" if $st != 0;
    return join( " ", @$lines );
}

# ---------------------------------------------------------------------------
# Suite lifecycle
# ---------------------------------------------------------------------------

sub testsuite_init {
    my ( $running, $st ) = capture( "pgrep", "emuxfs" );
    if ( $st == 0 && @$running ) {
        die "emuxfs already running\n";
    }
    foreach my $path ( $test_tmp, $dev_a, $dev_b, $dev_c, $mp ) {
        remove_tree( $path, { safe => 1 } ) if -e $path;
    }
    make_path($mp);
    $started = 1;
}

sub testsuite_final {
    rmdir($mp) if -d $mp;
    $started = 0;
}

sub pre_test {
    make_path( $test_tmp, $dev_a, $dev_b );
    must_emuxfs( "format", "-a", $chk_alg, $dev_a, $dev_b );
    must_emuxfs( "mount", $mp, $dev_a, $dev_b );
    $mounted = 1;
}

sub post_test {
    whether_mounted();
    remove_tree( $test_tmp, $dev_a, $dev_b, { safe => 1 } );
}

sub whether_mounted {
    if ($mounted) {
        quiet( "umount", $mp );
        $mounted = 0;
    }
}

END {
    whether_mounted();
}

# ---------------------------------------------------------------------------
# Dispatch tables (Perl hashes of code references)
# ---------------------------------------------------------------------------

my %TESTS;

# --- basic operations -------------------------------------------------------

$TESTS{test_mknod} = sub {
    spit( "$mp/r", "" );
    quiet( "cat", "$dev_a/r" ) == 0 or die "cat dev_a/r\n";
    quiet( "cat", "$dev_b/r" ) == 0 or die "cat dev_b/r\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_mkdir} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    must_quiet( "ls a/d", "ls", "$dev_a/d" );
    must_quiet( "ls b/d", "ls", "$dev_b/d" );
    must_quiet( "ls -l",  "ls", "-l", $mp );
};

$TESTS{test_symlink} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    must_quiet( "readlink a", "readlink", "$dev_a/l" );
    must_quiet( "readlink b", "readlink", "$dev_b/l" );
    must_quiet( "ls -l",      "ls",       "-l", $mp );
};

$TESTS{test_getattr} = sub {
    spit( "$mp/r", "" );
    mkdir("$mp/d")            or die "mkdir: $!\n";
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_write} = sub {
    spit( "$mp/r", "" );
    spit( "$mp/r", "foo\n" );
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_truncate} = sub {
    spit( "$mp/r", "foo\n" );
    spit( "$mp/r", "" );
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_chmod_reg} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_chmod_dir} = sub {
    mkdir("$mp/d")         or die "mkdir: $!\n";
    chmod( 0700, "$mp/d" ) or die "chmod: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_chmod_lnk} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    must_quiet( "chmod -h", "chmod", "-h", "0700", "$mp/l" );
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_chown_reg} = sub {
    spit( "$mp/r", "" );
    must_quiet( "chown", "chown", $unpriv_user, "$mp/r" );
    must_quiet( "ls -l", "ls",    "-l",         $mp );
};

$TESTS{test_chown_dir} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    must_quiet( "chown", "chown", $unpriv_user, "$mp/d" );
    must_quiet( "ls -l", "ls",    "-l",         $mp );
};

$TESTS{test_chown_lnk} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    must_quiet( "chown -h", "chown", "-h", $unpriv_user, "$mp/l" );
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_rename} = sub {
    spit( "$mp/r", "" );
    rename( "$mp/r", "$mp/r2" ) or die "rename: $!\n";
    quiet( "cat", "$dev_a/r2" ) == 0 or die "cat a/r2\n";
    quiet( "cat", "$dev_b/r2" ) == 0 or die "cat b/r2\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_read} = sub {
    spit( "$mp/r", "foo\n" );
    quiet( "cat", "$mp/r" ) == 0 or die "cat\n";
};

$TESTS{test_readlink} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    my $t = readlink("$mp/l");
    die "readlink\n" unless defined $t && $t eq "foo";
};

$TESTS{test_readdir} = sub {
    spit( "$mp/r", "" );
    mkdir("$mp/d")            or die "mkdir: $!\n";
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_unlink_reg} = sub {
    spit( "$mp/r", "" );
    unlink("$mp/r") or die "unlink: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_unlink_lnk} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    unlink("$mp/l")           or die "unlink: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_rmdir} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    rmdir("$mp/d") or die "rmdir: $!\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_lfile_write} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    system("sync");
    die "cmp a\n" if compare( "$test_tmp/r", "$dev_a/r" ) != 0;
    die "cmp b\n" if compare( "$test_tmp/r", "$dev_b/r" ) != 0;
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_lfile_read} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    die "cmp\n" if compare( "$test_tmp/r", "$mp/r" ) != 0;
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_lfile_truncate_large_to_small} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    spit( "$mp/r", "foo\n" );
    system("sync");
    die "a content\n" unless slurp("$dev_a/r") eq "foo\n";
    die "b content\n" unless slurp("$dev_b/r") eq "foo\n";
    must_quiet( "ls -l", "ls", "-l", $mp );
};

$TESTS{test_lfile_extend_large_to_larger} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    make_random_file( "$test_tmp/r2", 4096 );
    my $extra = slurp("$test_tmp/r2");
    append_to( "$test_tmp/r", $extra );
    append_to( "$mp/r",       $extra );
    system("sync");
    die "cmp a\n" if compare( "$test_tmp/r", "$dev_a/r" ) != 0;
    die "cmp b\n" if compare( "$test_tmp/r", "$dev_b/r" ) != 0;
    must_quiet( "ls -l", "ls", "-l", $mp );
};

# --- resilience -------------------------------------------------------------

$TESTS{test_resiliance_reg} = sub {
    spit( "$mp/r", "foo\n" );
    system("sync");
    spit( "$dev_a/r", "bad\n" );
    die "resilience reg\n" unless slurp("$mp/r") eq "foo\n";
};

$TESTS{test_resiliance_lnk} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    quiet( "ln", "-sf", "bar", "$dev_a/l" );
    my $t = readlink("$mp/l");
    die "resilience lnk\n" unless defined $t && $t eq "foo";
};

$TESTS{test_resiliance_dir} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    spit( "$mp/d/r", "foo\n" );
    system("sync");
    spit( "$dev_a/d/r", "bar\n" );
    rename( "$dev_a/d/r", "$dev_a/d/bad" ) or die "rename: $!\n";
    die "resilience dir listing\n" unless list_dir("$mp/d") eq "r";
    die "resilience dir content\n" unless slurp("$mp/d/r") eq "foo\n";
};

$TESTS{test_resiliance_lfile} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    system("sync");
    spit( "$dev_a/r", "bad\n" );
    die "resilience lfile\n" if compare( "$test_tmp/r", "$mp/r" ) != 0;
};

# --- restoration ------------------------------------------------------------

$TESTS{test_restoration_reg} = sub {
    spit( "$mp/r", "foo\n" );
    system("sync");
    spit( "$dev_a/r", "bad\n" );
    quiet( "cat", "$mp/r" );
    die "restoration reg\n" unless slurp("$dev_a/r") eq "foo\n";
};

$TESTS{test_restoration_lnk} = sub {
    symlink( "foo", "$mp/l" ) or die "symlink: $!\n";
    quiet( "ln", "-sf", "bad", "$dev_a/l" );
    quiet( "readlink", "$mp/l" );
    my $t = readlink("$dev_a/l");
    die "restoration lnk\n" unless defined $t && $t eq "foo";
};

$TESTS{test_restoration_dir} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    spit( "$mp/d/r", "foo\n" );
    system("sync");
    spit( "$dev_a/d/r", "bar\n" );
    rename( "$dev_a/d/r", "$dev_a/d/bad" ) or die "rename: $!\n";
    quiet( "ls", "$mp/d" );
    die "restoration dir listing\n" unless list_dir("$dev_a/d") eq "r";
    die "restoration dir content\n" unless slurp("$dev_a/d/r") eq "foo\n";
};

$TESTS{test_restoration_missing_reg} = sub {
    spit( "$mp/r", "foo\n" );
    system("sync");
    unlink("$dev_a/r") or die "unlink: $!\n";
    quiet( "cat", "$mp/r" );
    die "restoration missing reg\n" unless slurp("$dev_a/r") eq "foo\n";
};

$TESTS{test_restoration_lfile} = sub {
    make_random_file( "$test_tmp/r", 7168 );
    copy( "$test_tmp/r", "$mp/r" ) or die "copy: $!\n";
    system("sync");
    spit( "$dev_a/r", "bad\n" );
    quiet( "cat", "$mp/r" );
    die "restoration lfile\n" if compare( "$test_tmp/r", "$dev_a/r" ) != 0;
};

# --- negative operations ----------------------------------------------------

$TESTS{test_nonexistent} = sub {
    must_fail( "cat nonexistent", "cat", "$mp/nonexistent" );
    must_fail( "ls nonexistent",  "ls",  "$mp/nonexistent" );
};

$TESTS{test_broken_lnk} = sub {
    symlink( "nonexistent", "$mp/l" ) or die "symlink: $!\n";
    must_fail( "cat broken link", "cat", "$mp/l" );
};

$TESTS{test_not_a_reg} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    must_fail( "cat dir", "cat", "$mp/d" );
    symlink( "d", "$mp/l" ) or die "symlink: $!\n";
    must_fail( "cat link to dir", "cat", "$mp/l" );
};

$TESTS{test_not_a_dir} = sub {
    spit( "$mp/r", "foo\n" );
    must_fail( "ls reg", "ls", "$mp/r" );
    symlink( "r", "$mp/l" ) or die "symlink: $!\n";
    must_fail( "ls link", "ls", "$mp/l" );
};

$TESTS{test_not_a_lnk} = sub {
    spit( "$mp/r", "foo\n" );
    must_fail( "readlink reg", "readlink", "$mp/r" );
    mkdir("$mp/d") or die "mkdir: $!\n";
    must_fail( "readlink dir", "readlink", "$mp/d" );
};

# --- permissions ------------------------------------------------------------

sub as_unpriv {
    my ($cmd) = @_;
    return quiet( "su", "-l", "-s", "/bin/ksh", $unpriv_user, "-c", $cmd );
}

$TESTS{test_permission_cat} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission cat\n"
      if as_unpriv("cat $mp/r >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_append} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission append\n"
      if as_unpriv("echo bar >>$mp/r >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_ls} = sub {
    mkdir("$mp/d") or die "mkdir: $!\n";
    spit( "$mp/d/r1", "" );
    spit( "$mp/d/r2", "" );
    chmod( 0700, "$mp/d" ) or die "chmod: $!\n";
    die "permission ls\n"
      if as_unpriv("ls $mp/d >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_mv} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission mv\n"
      if as_unpriv("mv $mp/r $mp/r2 >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_rm} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission rm\n"
      if as_unpriv("rm -f $mp/r >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_chmod} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission chmod\n"
      if as_unpriv("chmod 0777 $mp/r >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_chown} = sub {
    spit( "$mp/r", "" );
    chmod( 0700, "$mp/r" ) or die "chmod: $!\n";
    die "permission chown\n"
      if as_unpriv("chown $unpriv_user $mp/r >/dev/null 2>&1") == 0;
};

$TESTS{test_permission_suid_chown} = sub {
    spit( "$mp/r", "echo foo\n" );
    chmod( 04644, "$mp/r" ) or die "chmod: $!\n";
    must_quiet( "chown", "chown", $unpriv_user, "$mp/r" );
    my ( $out, $st ) = capture( "stat", "-f", "%Sp", "$mp/r" );
    die "suid chown\n"
      unless $st == 0 && @$out && $out->[0] eq "-rw-r--r--";
};

$TESTS{test_permission_sgid_chown} = sub {
    spit( "$mp/r", "echo foo\n" );
    chmod( 02644, "$mp/r" ) or die "chmod: $!\n";
    must_quiet( "chown", "chown", $unpriv_user, "$mp/r" );
    my ( $out, $st ) = capture( "stat", "-f", "%Sp", "$mp/r" );
    die "sgid chown\n"
      unless $st == 0 && @$out && $out->[0] eq "-rw-r--r--";
};

# --- audit / heal / sync ----------------------------------------------------

$TESTS{test_audit} = sub {
    make_path( $dev_a, $dev_b, $test_tmp );
    spit( "$test_tmp/tmp1", "test line 1\n" );
    must_emuxfs( "format", "-a", $chk_alg, $dev_a, $dev_b );
    must_emuxfs( "mount", $mp, $dev_a, $dev_b );
    $mounted = 1;
    sleep_tick();
    spit( "$mp/r1", "foo1\n" );
    mkdir("$mp/d1") or die "mkdir: $!\n";
    spit( "$mp/d1/r2", "foo2\n" );
    symlink( "$test_tmp/tmp1", "$mp/d1/l1" ) or die "symlink: $!\n";
    whether_mounted();
    append_to( "$dev_a/d1/r2", "badline\n" );

    my ( $lines, $st ) = capture( $EMUXFS, "audit", $dev_a, $dev_b );
    my $pat = qr{\A\Q$dev_a\E/d1/r2\z};
    die "audit did not report d1/r2\n"
      unless $st == 0 && grep { $_ =~ $pat } @$lines;

    remove_tree( $dev_a, $dev_b, $test_tmp, { safe => 1 } );
};

$TESTS{test_heal} = sub {
    make_path( $dev_a, $dev_b, $test_tmp );
    spit( "$test_tmp/tmp1", "test line 1\n" );
    must_emuxfs( "format", "-a", $chk_alg, $dev_a, $dev_b );
    must_emuxfs( "mount", $mp, $dev_a, $dev_b );
    $mounted = 1;
    sleep_tick();
    spit( "$mp/r1", "foo1\n" );
    mkdir("$mp/d1") or die "mkdir: $!\n";
    spit( "$mp/d1/r2", "foo2\n" );
    symlink( "$test_tmp/tmp1", "$mp/d1/l1" ) or die "symlink: $!\n";
    whether_mounted();
    append_to( "$dev_a/d1/r2", "badline\n" );

    my ( $lines, $st ) = capture( $EMUXFS, "heal", $dev_a, $dev_b );
    my $pat = qr{\A\Q$dev_a\E/d1/r2\z};
    die "heal did not report d1/r2\n"
      unless $st == 0 && grep { $_ =~ $pat } @$lines;

    my $diff = quiet( "diff", "-r", "-x", ".muxfs", $dev_a, $dev_b );
    die "diff after heal\n" if $diff != 0;

    remove_tree( $dev_a, $dev_b, $test_tmp, { safe => 1 } );
};

$TESTS{test_sync} = sub {
    make_path( $dev_a, $dev_b, $dev_c, $test_tmp );
    spit( "$test_tmp/tmp1", "test line 1\n" );
    must_emuxfs( "format", "-a", $chk_alg, $dev_a, $dev_b );
    must_emuxfs( "mount", $mp, $dev_a, $dev_b );
    $mounted = 1;
    sleep_tick();
    spit( "$mp/r1", "foo1\n" );
    mkdir("$mp/d1") or die "mkdir: $!\n";
    spit( "$mp/d1/r2", "foo2\n" );
    symlink( "$test_tmp/tmp1", "$mp/d1/l1" ) or die "symlink: $!\n";
    whether_mounted();
    must_emuxfs( "sync", $dev_c, $dev_a, $dev_b );
    my $d1 = quiet( "diff", "-r", "-x", ".muxfs", $dev_c, $dev_a );
    my $d2 = quiet( "diff", "-r", "-x", ".muxfs", $dev_c, $dev_b );
    die "sync c != a\n" if $d1 != 0;
    die "sync c != b\n" if $d2 != 0;
    remove_tree( $dev_a, $dev_b, $dev_c, $test_tmp, { safe => 1 } );
};

# ---------------------------------------------------------------------------
# Groups
# ---------------------------------------------------------------------------

my @basic_tests = qw(
  test_mknod test_mkdir test_symlink test_getattr test_write
  test_truncate test_chmod_reg test_chmod_dir test_chmod_lnk
  test_chown_reg test_chown_dir test_chown_lnk test_rename test_read
  test_readlink test_readdir test_unlink_reg test_unlink_lnk test_rmdir
  test_lfile_write test_lfile_read test_lfile_truncate_large_to_small
  test_lfile_extend_large_to_larger
);

my @resiliance_tests = qw(
  test_resiliance_reg test_resiliance_lnk test_resiliance_dir
  test_resiliance_lfile
);

my @restoration_tests = qw(
  test_restoration_reg test_restoration_lnk test_restoration_dir
  test_restoration_missing_reg test_restoration_lfile
);

my @negative_tests = qw(
  test_nonexistent test_broken_lnk test_not_a_reg test_not_a_dir
  test_not_a_lnk
);

my @permission_tests = qw(
  test_permission_cat test_permission_append test_permission_ls
  test_permission_mv test_permission_rm test_permission_chmod
  test_permission_chown test_permission_suid_chown
  test_permission_sgid_chown
);

my @extra_tests = qw(test_audit test_heal test_sync);

sub run_tests {
    my (@names) = @_;
    foreach my $name (@names) {
        print "$name\n";
        die "unknown test $name\n" unless exists $TESTS{$name};
        pre_test();
        sleep_tick();
        $TESTS{$name}->();
        post_test();
    }
}

$TESTS{test_basics}      = sub { run_tests(@basic_tests) };
$TESTS{test_resiliance}  = sub { run_tests(@resiliance_tests) };
$TESTS{test_restoration} = sub { run_tests(@restoration_tests) };
$TESTS{test_negatives}   = sub { run_tests(@negative_tests) };
$TESTS{test_permissions} = sub {
    if ( $unpriv_user eq "" ) {
        print "SKIP: unpriv_user is not set; permission tests skipped\n";
        return;
    }
    run_tests(@permission_tests);
};
$TESTS{test_extras} = sub {
    foreach my $t (@extra_tests) {
        print "$t\n";
        $TESTS{$t}->();
    }
};

my @all_tests = qw(
  test_basics test_resiliance test_restoration test_negatives
  test_permissions test_extras
);

my $ok = eval {
    testsuite_init();
    foreach my $alg (qw(crc32 md5 sha1)) {
        print "Checksum algorithm: $alg\n";
        $chk_alg = $alg;
        foreach my $t (@all_tests) {
            print "$t\n";
            $TESTS{$t}->();
        }
    }
    testsuite_final();
    1;
};
if ( !$ok ) {
    my $err = $@ // "unknown failure";
    $err =~ s/\n\z//;
    print STDERR "$err\n";
    whether_mounted();
    exit 1;
}

print "All tests passing.\n";
exit 0;
