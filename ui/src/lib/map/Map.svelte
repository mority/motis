<script lang="ts">
	import maplibregl from 'maplibre-gl';
	import { setContext, type Snippet } from 'svelte';
	import 'maplibre-gl/dist/maplibre-gl.css';
	import { createShield } from './shield';

	let {
		map = $bindable(),
		zoom = $bindable(),
		// eslint-disable-next-line @typescript-eslint/no-unused-vars
		bounds = $bindable(),
		center = $bindable(),
		style,
		attribution,
		transformRequest,
		children,
		class: className
	}: {
		map?: maplibregl.Map;
		style: maplibregl.StyleSpecification | undefined;
		attribution: string | undefined | false;
		transformRequest?: maplibregl.RequestTransformFunction;
		center: maplibregl.LngLatLike;
		bounds?: maplibregl.LngLatBoundsLike | undefined;
		zoom: number;
		children?: Snippet;
		class: string;
	} = $props();

	let el: HTMLElement | null = null;
	let currStyle: maplibregl.StyleSpecification | undefined = style;
	let ctx = $state<{ map: maplibregl.Map | undefined }>({ map: undefined });
	setContext('map', ctx);

	let currentZoom = $state.snapshot(zoom);
	let currentCenter = $state.snapshot(center);

	const updateZoom = () => {
		if (map && $state.snapshot(zoom) !== currentZoom) {
			currentZoom = map.getZoom();
			map.setZoom(zoom);
		}
	};

	const updateCenter = () => {
		if (
			map &&
			center.toString() !=
				maplibregl.LngLat.convert(currentCenter as maplibregl.LngLatLike).toString()
		) {
			currentCenter = map.getCenter();
			map.setCenter(center);
		}
	};

	const updateStyle = () => {
		if (style != currStyle) {
			if (!ctx.map && el) {
				createMap(el);
			} else if (ctx.map) {
				ctx.map.setStyle(style || null);
			}
		}
	};

	const createMap = (container: HTMLElement) => {
		if (!style) {
			return;
		}
		let tmp: maplibregl.Map;
		try {
			tmp = new maplibregl.Map({
				container,
				zoom,
				bounds,
				center,
				style,
				transformRequest,
				attributionControl:
					attribution === false || attribution === undefined
						? attribution
						: { customAttribution: attribution }
			});

			tmp.addImage(
				'shield',
				...createShield({
					fill: 'hsl(0, 0%, 98%)',
					stroke: 'hsl(0, 0%, 75%)'
				})
			);

			tmp.addImage(
				'shield-dark',
				...createShield({
					fill: 'hsl(0, 0%, 16%)',
					stroke: 'hsl(0, 0%, 30%)'
				})
			);

			tmp.on('load', () => {
				map = tmp;
				ctx.map = tmp;
				updateZoom();
				updateCenter();
				bounds = tmp.getBounds();
				currStyle = style;

				tmp.on('moveend', () => {
					zoom = tmp.getZoom();
					currentZoom = zoom;

					center = tmp.getCenter();
					currentCenter = center;

					bounds = tmp.getBounds();
				});
			});
		} catch (e) {
			console.log(e);
		}

		return {
			destroy() {
				tmp?.remove();
				ctx.map = undefined;
			}
		};
	};

	$effect(updateStyle);
	$effect(updateZoom);
	$effect(updateCenter);
</script>

<div use:createMap bind:this={el} class={className}>
	{#if children}
		{@render children()}
	{/if}
</div>
