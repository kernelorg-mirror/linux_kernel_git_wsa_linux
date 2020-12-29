#! /usr/bin/perl
use strict;
use autodie;

sub numerically { $a <=> $b };
$" = ", ";

my %timestamps;
my $n_chan = 0;
my @chan_names;
my @cur_state;
my $delta = 100; # 1000ns

open my $fh, '<', $ARGV[1];

while ( <$fh> ) {
	if ( /^Channel (\d+): (.*)$/ ) {
		$chan_names[$1] = $2;
		die "Unexpected channel number '$1'\n" if ($n_chan != $1);
		$n_chan++;
	}
}
close $fh;
print "time, @chan_names\n";

open my $fh, '<:raw', $ARGV[0];

while ( read $fh, my $quad, 8 ) {
	my $data = unpack 'Q', $quad;
	my $ts = $data >> 8;
	my $chan = ($data >> 1) & 127;
	my $val = $data & 1;

	$timestamps{$ts}{$chan} = $val;
}
close $fh;

my $n_samples = 0;
my $ts0 = 0;
my $t = 0;

foreach my $ts (sort numerically keys %timestamps) {
	my $chan_set;

	foreach my $chan ( sort keys %{ $timestamps{$ts} } ) {
		while ($ts0 && $ts - $ts0 > $t) {
			print $t * 10**-9, ", @cur_state\n";
			$t += $delta;
		}

		print STDERR "$ts $chan $timestamps{$ts}{$chan}\n";

		$cur_state[$chan] = $timestamps{$ts}{$chan};
		$n_samples++;
		if ($n_samples == $n_chan) {
			$ts0 = $ts;
			#print "0.0, @cur_state\n";
		}
	}
}
print $t * 10**-9, ", @cur_state\n";


# SI unit converter
# %c=(f,-15,p,-12,n,-9,µ,-6,'m',-3,c,-2,h,2,k,3,M,6,G,9,T,12,P,15);/([0-9.]+) (.).*(.)./;$_=$1*10**($c{$2}-$c{$3})
