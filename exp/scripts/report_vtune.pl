#!/usr/bin/perl

use strict; 
use warnings;
use Getopt::Long;

my $InType = "line";
my %validInTypes = map { $_ => 1 } ("line", "function");
my $showType = "raw";
my %validShowTypes = map { $_ => 1 } ("raw", "ratio", "scalebythread");

my %Stats = ();
my %Thread_keys = ();
my $TOTAL = "TOTAL";

sub show {
  my ($tmap, $lk, $tk) = @_;

  if (!exists $tmap->{$lk}{$tk}) {
    print STDERR "ERROR: missing key: tk=$tk, lk=$lk\n";
  }

  if ($showType eq "scalebythread") {
    return $tmap->{$lk}{$tk} / $tk;
  } elsif ($showType eq "ratio") {
    return $tmap->{$lk}{$tk} / $tmap->{$TOTAL}{$tk};
  } elsif ($showType eq "raw") {
    return $tmap->{$lk}{$tk};
  } else {
    die;
  }

}

GetOptions('show=s'=>\$showType, 'in=s'=>\$InType) or die;
die("unknown show type") unless ($validShowTypes{$showType});
die("unknown in type") unless ($validInTypes{$InType});

my $newSet = 0;
my $curThread = 0;
my @heads = ();

while (<>) {
  chomp;
  my @line = split /\t/;

  # print "line:@line\n";
  # print "line:$line[0],$line[1]\n";


  if ($line[0] =~ /^THREADS$/) {
    # print "Threads line: @line\n";
    $newSet = 1;
    $curThread = $line[1];
    $Thread_keys{$curThread} = 1;

  } elsif ($newSet) {
    $newSet = 0;
    @heads = @line;

    # print "headers:@heads\n";

  } else {
    my $ind;
    my $offset = 0;

    if ($InType eq "line") {
      my $file = shift @line;
      my $ln = shift @line;
      my $module = shift @line;
      my $path = shift @line;

      $offset = 4;
      $ind = "$module:$file:$ln";

    } elsif ($InType eq "function") {
      my $function = shift @line;
      my $module = shift @line;

      $offset = 2;
      $function =~ s/,/;/g;
      $ind = "$module:$function";
    }


    for (my $i = 0; $i < $#line; $i++) {

      my $nk = $heads[$i + $offset];
      $Stats{$nk}{$curThread}{$ind} = $line[$i];
      $Stats{$nk}{$curThread}{$TOTAL} += $line[$i];

      # print "nk=$nk, val=$line[$i]\n";
    }
    # print "###\n";
  }
}



# for the combinations of (line_keys, Thread_keys) for a given stat_name that don't
# have corresponding Stats, we put a 0. e.g. a particular function/line shows
# up in the profile at threads=1 but not at threads=16.
foreach my $nk (keys %Stats) {
  my %line_keys = ();
  foreach my $tk (keys %{$Stats{$nk}}) {
    foreach my $lk (keys %{$Stats{$nk}{$tk}}) {
      $line_keys{$lk} = 1;
    }
  }

  foreach my $tk (keys %Thread_keys) {
    foreach my $lk (keys %line_keys) {
      if (!exists $Stats{$nk}{$tk}{$lk}) {
        $Stats{$nk}{$tk}{$lk} = 0;
      }    
    }
  }
}


my $maxThread = (sort { $a <=> $b } keys %Thread_keys)[-1];

foreach my $nk (sort keys %Stats) {
  print "$nk";
  foreach my $tk (sort { $a <=> $b } keys %Thread_keys) {
    print ",$tk";
  }
  print "\n";

  my %transpose = ();
  foreach my $tk (keys %Thread_keys) {
    foreach my $lk (keys %{$Stats{$nk}{$tk}}) {
      $transpose{$lk}{$tk} = $Stats{$nk}{$tk}{$lk};
    }
  }


  # delete lines with all 0s from transpose
  foreach my $lk (keys %transpose) {
    my $all_zeros = 1;
    foreach my $tk (keys %Thread_keys) {
      if ($transpose{$lk}{$tk} != 0) {
        $all_zeros = 0;
        last;
      }
    }

    if ($all_zeros) {
      delete $transpose{$lk};
    }
  }


  #sort by final thread performance
  foreach my $lk (sort { show(\%transpose, $b, $maxThread) <=> show(\%transpose, $a, $maxThread) }
    keys %transpose) {

    print "$lk";
    foreach my $tk (sort { $a <=> $b } keys %Thread_keys) {
      print "," . show(\%transpose, $lk, $tk);
    }
    print "\n";
  }

  print "\n\n\n";
}