<script setup lang="ts">
/**
 * The home-page mark. Same geometry as `public/logo.svg`, plus motion the
 * static file deliberately does not carry.
 *
 * It runs forever on a 3.6s cycle: a charge travels around the bowl, reaches
 * the opening the bolt sits in, and discharges — the bolt's halo swells, a
 * white copy blows out for a few frames, and a shockwave leaves the counter.
 * Then the charge comes back around. The phases are tuned against each other,
 * so the flash lands at the moment the charge disappears into the gap rather
 * than on an arbitrary beat.
 *
 * No filters are used in the loop. A blur would have to recomposite on every
 * frame; the travelling light is a radial-gradient blob clipped to the bowl
 * and the bolt's halo is a stroked copy, so the whole thing is transform and
 * opacity only.
 */
</script>

<template>
  <svg
    class="HeroLogo"
    viewBox="0 0 120 120"
    xmlns="http://www.w3.org/2000/svg"
    role="img"
    aria-label="react-native-quickjs"
  >
    <defs>
      <linearGradient id="hBowl" x1=".05" y1="0" x2=".95" y2="1">
        <stop offset="0" stop-color="#FBBF24" />
        <stop offset="1" stop-color="#EF4444" />
      </linearGradient>
      <linearGradient id="hShade" x1=".05" y1="0" x2=".95" y2="1">
        <stop offset="0" stop-color="#B45309" />
        <stop offset="1" stop-color="#991B1B" />
      </linearGradient>
      <radialGradient id="hSpark">
        <stop offset="0" stop-color="#FFF7E0" stop-opacity=".95" />
        <stop offset=".45" stop-color="#FFD9A0" stop-opacity=".45" />
        <stop offset="1" stop-color="#FFD9A0" stop-opacity="0" />
      </radialGradient>

      <mask id="hGap">
        <rect width="120" height="120" fill="#fff" />
        <polygon
          points="3,-11 -8,8 0,8 -4,45 8,2 1,2"
          transform="translate(60,59) rotate(-45)"
          fill="#000"
          stroke="#000"
          stroke-width="6"
          stroke-linejoin="round"
        />
      </mask>

      <clipPath id="hClip" clipPathUnits="userSpaceOnUse">
        <path
          transform="translate(6,7)"
          d="M59.73,84.50 A33,33 0 0 1 21.50,57.73 A33,33 0 0 1 48.27,19.50
             A33,33 0 0 1 86.50,46.27 A33,33 0 0 1 86.50,57.73
             L70.74,54.95 A17,17 0 0 0 56.95,35.26 A17,17 0 0 0 37.26,49.05
             A17,17 0 0 0 51.05,68.74 A17,17 0 0 0 56.95,68.74 Z"
        />
      </clipPath>
    </defs>

    <rect class="tile" width="120" height="120" rx="27" fill="#1A0E0A" />

    <g class="bowl">
      <g clip-path="url(#hClip)" mask="url(#hGap)">
        <polygon points="0,0 120,0 0,120" fill="url(#hBowl)" />
        <polygon points="120,0 120,120 0,120" fill="url(#hShade)" />
        <path d="M120,0 L0,120" fill="none" stroke="#1A0E0A" stroke-width="1.8" opacity=".45" />

        <!-- The charge. Sits on the annulus centreline (r=25) at 80°, just past
             the trailing edge of the opening, and rotates about the bowl centre.
             Clipped to the bowl, so it vanishes when it reaches the gap. -->
        <g class="orbit">
          <circle cx="64.3" cy="83.6" r="17" fill="url(#hSpark)" />
        </g>
      </g>
    </g>

    <!-- discharge: shockwave out of the counter, once per cycle -->
    <circle class="wave" opacity="0" cx="60" cy="59" r="24" fill="none" stroke="#FFF7E0" stroke-width="2.5" />

    <!-- the halo swells as the charge arrives -->
    <g class="halo" opacity="0">
      <polygon
        points="3,-11 -8,8 0,8 -4,45 8,2 1,2"
        transform="translate(60,59) rotate(-45)"
        fill="none"
        stroke="#FFD08A"
        stroke-width="6"
        stroke-linejoin="round"
      />
    </g>

    <g class="bolt">
      <polygon points="3,-11 -8,8 0,8 -4,45 8,2 1,2" transform="translate(60,59) rotate(-45)" fill="#FFF7E0" />
    </g>

    <!-- one-shot: the arrival strike, so the intro reads as causal -->
    <g class="strike" opacity="0">
      <polygon points="3,-11 -8,8 0,8 -4,45 8,2 1,2" transform="translate(60,59) rotate(-45)" fill="#fff" />
    </g>

    <!-- every cycle after that -->
    <g class="blowout" opacity="0">
      <polygon points="3,-11 -8,8 0,8 -4,45 8,2 1,2" transform="translate(60,59) rotate(-45)" fill="#fff" />
    </g>
  </svg>
</template>

<style scoped>
.HeroLogo {
  width: 100%;
  max-width: 280px;
  height: auto;
  display: block;
  filter: drop-shadow(0 22px 44px rgb(0 0 0 / 0.45));
}

/* Below the two-column breakpoint the mark sits above the sentence rather than
   beside it, so it has to leave room for the text instead of filling the
   column. At 280px it takes most of a phone screen on its own. */
@media (max-width: 959px) {
  .HeroLogo {
    max-width: 200px;
  }
}

@media (max-width: 479px) {
  .HeroLogo {
    max-width: 168px;
  }
}

/* Each group animates its own wrapper — the polygons keep their attribute
   transforms, and a CSS transform on the same element would replace them. */
.tile,
.bowl,
.bolt,
.strike,
.blowout,
.halo,
.wave {
  transform-box: fill-box;
  transform-origin: center;
}

/* The charge rotates about the bowl centre in user units, not about its own
   box, so this one needs the viewBox as its reference. */
.orbit {
  transform-box: view-box;
  transform-origin: 60px 59px;
}

/* ── intro, once ──────────────────────────────────────────────────────── */
.tile   { animation: tile-in 0.7s cubic-bezier(0.2, 0.85, 0.25, 1) both; }
.bowl   { animation: bowl-in 0.85s 0.12s cubic-bezier(0.2, 0.85, 0.25, 1) both; }
.bolt   { animation: bolt-in 0.45s 0.62s cubic-bezier(0.25, 1.5, 0.45, 1) both; }
.strike { animation: strike 0.4s 0.72s ease-out both; }

/* ── the forever loop ─────────────────────────────────────────────────── *
 * One 3.6s cycle, shared delay so the phases stay locked together. The
 * charge covers the bowl over 0–81% and is inside the gap for the rest,
 * which is why the discharge keyframes all sit at 80%.                    */
.orbit   { animation: orbit 3.6s 1.15s linear infinite; }
.halo    { animation: halo 3.6s 1.15s ease-out infinite; }
.blowout { animation: blowout 3.6s 1.15s ease-out infinite; }
.wave    { animation: wave 3.6s 1.15s ease-out infinite; }

@keyframes tile-in {
  from { opacity: 0; transform: scale(0.94); }
  to   { opacity: 1; transform: none; }
}

@keyframes bowl-in {
  from { opacity: 0; transform: scale(0.82) rotate(-14deg); }
  to   { opacity: 1; transform: none; }
}

/* travels in along its own -45° axis, so it reads as a strike, not a fade */
@keyframes bolt-in {
  from { opacity: 0; transform: translate(-20px, -20px); }
  to   { opacity: 1; transform: none; }
}

@keyframes strike {
  0%   { opacity: 0; transform: translate(-20px, -20px); }
  55%  { opacity: 0.85; transform: none; }
  100% { opacity: 0; transform: none; }
}

@keyframes orbit {
  to { transform: rotate(360deg); }
}

@keyframes halo {
  0%, 62%   { opacity: 0; transform: scale(1); }
  80%       { opacity: 0.55; transform: scale(1.04); }
  100%      { opacity: 0; transform: scale(1.14); }
}

@keyframes blowout {
  0%, 76%  { opacity: 0; }
  80%      { opacity: 0.9; }
  88%, 100% { opacity: 0; }
}

@keyframes wave {
  0%, 78%  { opacity: 0; transform: scale(0.5); }
  81%      { opacity: 0.6; }
  100%     { opacity: 0; transform: scale(2.1); }
}

/* Nothing moves for anyone who has asked for that — including the loop. */
@media (prefers-reduced-motion: reduce) {
  .tile,
  .bowl,
  .bolt,
  .strike,
  .blowout,
  .halo,
  .wave,
  .orbit {
    animation: none;
  }
  .strike,
  .blowout,
  .halo,
  .wave,
  .orbit {
    display: none;
  }
}
</style>
