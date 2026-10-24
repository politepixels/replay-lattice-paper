(* Copyright 2026 PolitePixels Limited

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. *)

Require Import List.
Require Import Arith.
Require Import Lia.
Import ListNotations.

Inductive lane : Type :=
  | Exact  : lane
  | Replay : lane
  | Approx : lane.

Definition join (a b : lane) : lane :=
  match a, b with
  | Approx, _      => Approx
  | _, Approx      => Approx
  | Replay, _      => Replay
  | _, Replay      => Replay
  | Exact, Exact   => Exact
  end.

Definition lane_le (a b : lane) : Prop := join a b = b.

Infix "<=l" := lane_le (at level 70).

Lemma join_idem : forall a, join a a = a.
Proof. destruct a; reflexivity. Qed.

Lemma join_exact_l : forall a, join Exact a = a.
Proof. destruct a; reflexivity. Qed.

Lemma exact_bottom : forall a, Exact <=l a.
Proof. intro a; unfold lane_le; apply join_exact_l. Qed.

Lemma join_approx_l : forall a, join Approx a = Approx.
Proof. destruct a; reflexivity. Qed.

Lemma le_refl : forall a, a <=l a.
Proof. intro a; unfold lane_le; apply join_idem. Qed.

Lemma le_trans : forall a b c, a <=l b -> b <=l c -> a <=l c.
Proof. destruct a, b, c; unfold lane_le; simpl; congruence. Qed.

Lemma join_ub_l : forall a b, a <=l join a b.
Proof. destruct a, b; reflexivity. Qed.

Lemma join_ub_r : forall a b, b <=l join a b.
Proof. destruct a, b; reflexivity. Qed.

Lemma join_lub : forall a b c, a <=l c -> b <=l c -> join a b <=l c.
Proof. destruct a, b, c; unfold lane_le; simpl; congruence. Qed.

Parameter schedule : Type.
Parameter Gated : schedule -> Prop.

Parameter oracle : schedule.
Axiom oracle_ungated : ~ Gated oracle.
Axiom some_gated : exists s, Gated s.

Parameter world : Type.

Definition agrees (l : lane) (s s' : schedule) : Prop :=
  match l with
  | Exact  => True
  | Replay => Gated s /\ Gated s'
  | Approx => False
  end.

Lemma agrees_exact_strictly_contains_replay :
  (forall s s', agrees Replay s s' -> agrees Exact s s') /\
  (exists s s', agrees Exact s s' /\ ~ agrees Replay s s').
Proof.
  split.
  - intros s s' _. exact I.
  - exists oracle, oracle. split; [ exact I | ].
    intros [Hg _]. exact (oracle_ungated Hg).
Qed.

Lemma agrees_replay_strictly_contains_approx :
  (forall s s', agrees Approx s s' -> agrees Replay s s') /\
  (exists s s', agrees Replay s s' /\ ~ agrees Approx s s').
Proof.
  split.
  - intros s s' [].
  - destruct some_gated as [s Hs]. exists s, s. split; [ split; exact Hs | intros [] ].
Qed.

Lemma agrees_antitone :
  forall a b s s', a <=l b -> agrees b s s' -> agrees a s s'.
Proof.
  destruct a, b; unfold lane_le; simpl; try tauto; discriminate.
Qed.

Record node : Type := mkNode {
  op_lane  : lane;
  inputs   : list nat;
  declared : option lane;

  source   : bool;

  carried  : option nat
}.

Definition prog := list node.

Definition wf (P : prog) : Prop :=
  forall i n j, nth_error P i = Some n -> In j (inputs n) -> j < i.

Parameter bits : Type.

Parameter eval : prog -> nat -> schedule -> world -> bits.

Definition Assumption1 (P : prog) : Prop :=
  forall i n s s' w,
    nth_error P i = Some n ->
    agrees (op_lane n) s s' ->
    (forall j, In j (inputs n) -> eval P j s w = eval P j s' w) ->
    (forall c, carried n = Some c -> eval P c s w = eval P c s' w) ->
    eval P i s w = eval P i s' w.

Definition carried_lane (L : nat -> lane) (n : node) : lane :=
  match carried n with
  | Some c => L c
  | None   => Exact
  end.

Definition node_lane (L : nat -> lane) (n : node) : lane :=
  join (carried_lane L n) (fold_left join (map L (inputs n)) (op_lane n)).

Definition sound_assignment (P : prog) (L : nat -> lane) : Prop :=
  forall i n, nth_error P i = Some n -> L i = node_lane L n.

Lemma fold_join_ge_seed :
  forall l a, a <=l fold_left join l a.
Proof.
  induction l as [| x xs IH]; intro a; simpl.
  - apply le_refl.
  - eapply le_trans; [ apply join_ub_l | apply IH ].
Qed.

Lemma fold_join_ge_elem :
  forall l a x, In x l -> x <=l fold_left join l a.
Proof.
  induction l as [| y ys IH]; intros a x Hin; simpl in *.
  - contradiction.
  - destruct Hin as [-> | Hin].
    + eapply le_trans; [ apply join_ub_r | apply fold_join_ge_seed ].
    + apply IH; exact Hin.
Qed.

Lemma node_lane_ge_op : forall L n, op_lane n <=l node_lane L n.
Proof.
  intros L n; unfold node_lane.
  eapply le_trans; [ apply fold_join_ge_seed | apply join_ub_r ].
Qed.

Lemma node_lane_ge_input :
  forall L n j, In j (inputs n) -> L j <=l node_lane L n.
Proof.
  intros L n j Hin; unfold node_lane.
  eapply le_trans; [ apply fold_join_ge_elem, in_map, Hin | apply join_ub_r ].
Qed.

Lemma node_lane_ge_carried :
  forall L n c, carried n = Some c -> L c <=l node_lane L n.
Proof.
  intros L n c Hc; unfold node_lane, carried_lane; rewrite Hc.
  apply join_ub_l.
Qed.

Definition carried_agrees (P : prog) (L : nat -> lane) (s s' : schedule) (w : world) : Prop :=
  forall i n c,
    nth_error P i = Some n ->
    carried n = Some c ->
    agrees (L i) s s' ->
    eval P c s w = eval P c s' w.

Theorem theorem1 :
  forall P L,
    wf P ->
    Assumption1 P ->
    sound_assignment P L ->
    forall i n s s' w,
      nth_error P i = Some n ->
      carried_agrees P L s s' w ->
      agrees (L i) s s' ->
      eval P i s w = eval P i s' w.
Proof.
  intros P L Hwf HA1 Hsound i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n s s' w Hnth Hcar Hag.
  rewrite (Hsound i n Hnth) in Hag.
  apply (HA1 i n s s' w Hnth).
  -
    eapply agrees_antitone; [ apply node_lane_ge_op | exact Hag ].
  -
    intros j Hin.
    assert (Hlt : j < i) by exact (Hwf i n j Hnth Hin).
    destruct (nth_error P j) as [nj |] eqn:Hj.
    + apply (IH j Hlt nj s s' w Hj Hcar).
      eapply agrees_antitone; [ apply (node_lane_ge_input L n j Hin) | exact Hag ].
    +
      exfalso.
      apply nth_error_None in Hj.
      assert (Hlen : i < length P)
        by (apply nth_error_Some; rewrite Hnth; discriminate).
      lia.
  -
    intros c Hc.
    apply (Hcar i n c Hnth Hc). rewrite (Hsound i n Hnth). exact Hag.
Qed.

Definition grounded_assignment (P : prog) (G : nat -> bool) : Prop :=
  forall i n,
    nth_error P i = Some n ->
    G i = andb (negb (source n))
               (andb (forallb G (inputs n))
                     (match carried n with Some c => G c | None => true end)).

Definition BlessHolds (P : prog) (B : nat -> bool) (s : schedule) (w w' : world) : Prop :=
  forall i, B i = true -> eval P i s w = eval P i s w'.

Definition waived_assignment (P : prog) (B C : nat -> bool) : Prop :=
  forall i n,
    nth_error P i = Some n ->
    C i = andb (orb (negb (source n)) (B i))
               (andb (forallb C (inputs n))
                     (match carried n with Some c => C c | None => true end)).

Lemma grounded_is_waived_unblessed :
  forall P G, grounded_assignment P G <-> waived_assignment P (fun _ => false) G.
Proof.
  intros P G. unfold grounded_assignment, waived_assignment. split;
    intros H i n Hnth; specialize (H i n Hnth); rewrite H; simpl;
    destruct (source n); reflexivity.
Qed.

Definition Assumption2 (P : prog) : Prop :=
  forall i n s w w',
    nth_error P i = Some n ->
    source n = false ->
    (forall j, In j (inputs n) -> eval P j s w = eval P j s w') ->
    (forall c, carried n = Some c -> eval P c s w = eval P c s w') ->
    eval P i s w = eval P i s w'.

Definition carried_world_agrees (P : prog) (G : nat -> bool) (s : schedule) (w w' : world) : Prop :=
  forall i n c,
    nth_error P i = Some n ->
    carried n = Some c ->
    G c = true ->
    eval P c s w = eval P c s w'.

Theorem waived_sound :
  forall P B C,
    wf P ->
    Assumption2 P ->
    waived_assignment P B C ->
    forall i n s w w',
      nth_error P i = Some n ->
      BlessHolds P B s w w' ->
      carried_world_agrees P C s w w' ->
      C i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P B C Hwf HA2 Hcov i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n s w w' Hnth Hbless Hcar Hc.
  rewrite (Hcov i n Hnth) in Hc.
  apply andb_prop in Hc. destruct Hc as [Hhead Hrest].
  apply andb_prop in Hrest. destruct Hrest as [Hins Hcarried].
  destruct (B i) eqn:Hb.
  -
    exact (Hbless i Hb).
  -
    assert (Hsrc : source n = false)
      by (destruct (source n); [ simpl in Hhead; discriminate | reflexivity ]).
    apply (HA2 i n s w w' Hnth).
    + exact Hsrc.
    +
      intros j Hin.
      assert (Hlt : j < i) by exact (Hwf i n j Hnth Hin).
      destruct (nth_error P j) as [nj |] eqn:Hj.
      * apply (IH j Hlt nj s w w' Hj Hbless Hcar).
        rewrite forallb_forall in Hins. exact (Hins j Hin).
      * exfalso.
        apply nth_error_None in Hj.
        assert (Hlen : i < length P)
          by (apply nth_error_Some; rewrite Hnth; discriminate).
        lia.
    +
      intros c Hc'.
      apply (Hcar i n c Hnth Hc').
      rewrite Hc' in Hcarried. exact Hcarried.
Qed.

Theorem grounding_sound :
  forall P G,
    wf P ->
    Assumption2 P ->
    grounded_assignment P G ->
    forall i n s w w',
      nth_error P i = Some n ->
      carried_world_agrees P G s w w' ->
      G i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P G Hwf HA2 Hg i n s w w' Hnth Hcar Hgi.
  apply (waived_sound P (fun _ => false) G Hwf HA2
           (proj1 (grounded_is_waived_unblessed P G) Hg) i n s w w' Hnth).
  -
    intros j Hj. discriminate.
  - exact Hcar.
  - exact Hgi.
Qed.

Definition lone_source : node := mkNode Exact [] None true None.

Lemma blessed_source_is_waived :
  forall C, waived_assignment [lone_source] (fun _ => true) C -> C 0 = true.
Proof. intros C H. specialize (H 0 lone_source eq_refl). exact H. Qed.

Lemma unblessed_source_is_not_grounded :
  forall G, grounded_assignment [lone_source] G -> G 0 = false.
Proof. intros G H. specialize (H 0 lone_source eq_refl). exact H. Qed.

Definition grounding_accepted (Demands C : nat -> bool) : Prop :=
  forall i, Demands i = true -> C i = true.

Corollary waived_declaration_sound :
  forall P B C Demands,
    wf P ->
    Assumption2 P ->
    waived_assignment P B C ->
    grounding_accepted Demands C ->
    forall i n s w w',
      nth_error P i = Some n ->
      BlessHolds P B s w w' ->
      carried_world_agrees P C s w w' ->
      Demands i = true ->
      eval P i s w = eval P i s w'.
Proof.
  intros P B C Demands Hwf HA2 Hcov Hacc i n s w w' Hnth Hbless Hcar Hd.
  apply (waived_sound P B C Hwf HA2 Hcov i n s w w' Hnth Hbless Hcar).
  exact (Hacc i Hd).
Qed.

Theorem theorem2_non_recovery :
  forall (L : nat -> lane) (n : node) (u : nat),
    In u (inputs n) \/ carried n = Some u ->
    L u = Approx ->
    node_lane L n = Approx.
Proof.
  intros L n u Hreads Hu.
  assert (H : Approx <=l node_lane L n).
  { rewrite <- Hu. destruct Hreads as [Hin | Hc].
    - apply node_lane_ge_input, Hin.
    - apply node_lane_ge_carried, Hc. }
  unfold lane_le in H. rewrite join_approx_l in H. congruence.
Qed.

Definition derivable (D : nat -> lane) (P : prog) : Prop :=
  forall i n, nth_error P i = Some n -> node_lane D n <=l D i.

Inductive has_lane (P : prog) : nat -> lane -> Prop :=
  | HL_join :
      forall i n (f : nat -> lane),
        nth_error P i = Some n ->
        carried n = None ->
        (forall j, In j (inputs n) -> has_lane P j (f j)) ->
        has_lane P i (node_lane f n)
  | HL_sub :
      forall i l l',
        has_lane P i l -> l <=l l' -> has_lane P i l'.

Definition acyclic (P : prog) : Prop :=
  forall i n, nth_error P i = Some n -> carried n = None.

Lemma lemma1_sound :
  forall P L,
    wf P ->
    acyclic P ->
    sound_assignment P L ->
    forall i n, nth_error P i = Some n -> has_lane P i (L i).
Proof.
  intros P L Hwf Hacy Hsound i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n Hnth.
  rewrite (Hsound i n Hnth).
  apply (HL_join P i n L Hnth (Hacy i n Hnth)).
  intros j Hin.
  assert (Hlt : j < i) by (eapply Hwf; eauto).
  destruct (nth_error P j) as [nj |] eqn:Hj.
  - apply (IH j Hlt nj Hj).
  - exfalso.

    apply nth_error_None in Hj.
    assert (Hi : i < length P) by (apply nth_error_Some; rewrite Hnth; discriminate).
    lia.
Qed.

Lemma fold_join_mono :
  forall l (f g : nat -> lane) a b,
    (forall x, In x l -> f x <=l g x) ->
    a <=l b ->
    fold_left join (map f l) a <=l fold_left join (map g l) b.
Proof.
  induction l as [| x xs IH]; intros f g a b Hpt Hab; simpl.
  - exact Hab.
  - apply IH.
    + intros y Hin. apply Hpt. right. exact Hin.
    + apply join_lub.
      * eapply le_trans; [ exact Hab | apply join_ub_l ].
      * eapply le_trans; [ apply Hpt; left; reflexivity | apply join_ub_r ].
Qed.

Lemma lemma1_least :
  forall P L,
    wf P ->
    sound_assignment P L ->
    forall i l, has_lane P i l -> L i <=l l.
Proof.
  intros P L Hwf Hsound i l Hd.
  induction Hd as [i n f Hnth Hnc Hins IH | i l l' Hd IH Hle].
  - rewrite (Hsound i n Hnth).
    unfold node_lane, carried_lane; rewrite Hnc.
    apply join_lub.
    + apply exact_bottom.
    + eapply le_trans; [| apply join_ub_r ].
      apply fold_join_mono; [| apply le_refl ].
      intros x Hin. apply (IH x Hin).
  - eapply le_trans; [ exact IH | exact Hle ].
Qed.

Lemma join_mono :
  forall a b c d, a <=l c -> b <=l d -> join a b <=l join c d.
Proof.
  intros a b c d Hac Hbd.
  apply join_lub.
  - eapply le_trans; [ exact Hac | apply join_ub_l ].
  - eapply le_trans; [ exact Hbd | apply join_ub_r ].
Qed.

Lemma carried_lane_mono :
  forall (L M : nat -> lane) n,
    (forall j, L j <=l M j) ->
    carried_lane L n <=l carried_lane M n.
Proof.
  intros L M n Hpt. unfold carried_lane.
  destruct (carried n); [ apply Hpt | apply le_refl ].
Qed.

Lemma node_lane_mono :
  forall (L M : nat -> lane) n,
    (forall j, L j <=l M j) ->
    node_lane L n <=l node_lane M n.
Proof.
  intros L M n Hpt. unfold node_lane. apply join_mono.
  - apply carried_lane_mono, Hpt.
  - apply fold_join_mono; [ intros x _; apply Hpt | apply le_refl ].
Qed.

Lemma node_lane_ext :
  forall (L M : nat -> lane) n,
    (forall j, L j = M j) ->
    node_lane L n = node_lane M n.
Proof.
  intros L M n Hpt. unfold node_lane, carried_lane.
  assert (Hmap : map L (inputs n) = map M (inputs n)) by (apply map_ext; exact Hpt).
  rewrite Hmap.
  destruct (carried n) as [c |]; rewrite ?Hpt; reflexivity.
Qed.

Definition step (P : prog) (L : nat -> lane) : nat -> lane :=
  fun i =>
    match nth_error P i with
    | Some n => node_lane L n
    | None   => Exact
    end.

Fixpoint iter (P : prog) (k : nat) : nat -> lane :=
  match k with
  | O    => fun _ => Exact
  | S k' => step P (iter P k')
  end.

Lemma iter_ascending :
  forall P k i, iter P k i <=l iter P (S k) i.
Proof.
  intros P k. induction k as [| k IH]; intros i.
  - apply exact_bottom.
  - simpl. unfold step. destruct (nth_error P i) as [n |] eqn:Hn.
    + apply node_lane_mono. intros j. apply IH.
    + apply le_refl.
Qed.

Lemma iter_stable_forever :
  forall P k,
    (forall i, iter P (S k) i = iter P k i) ->
    forall m i, iter P (k + m) i = iter P k i.
Proof.
  intros P k Hstab m. induction m as [| m IH]; intros i.
  - rewrite Nat.add_0_r. reflexivity.
  - replace (k + S m) with (S (k + m)) by lia. simpl.
    unfold step. destruct (nth_error P i) as [n |] eqn:Hn.
    + rewrite (node_lane_ext (iter P (k + m)) (iter P k) n IH).
      rewrite <- Hstab. simpl. unfold step. rewrite Hn. reflexivity.
    + rewrite <- Hstab. simpl. unfold step. rewrite Hn. reflexivity.
Qed.

Lemma iter_oob :
  forall P k i, length P <= i -> iter P k i = Exact.
Proof.
  intros P k i Hle. destruct k as [| k]; [ reflexivity | ].
  simpl. unfold step. rewrite (proj2 (nth_error_None P i) Hle). reflexivity.
Qed.

Definition height (l : lane) : nat :=
  match l with
  | Exact  => 0
  | Replay => 1
  | Approx => 2
  end.

Lemma height_mono : forall a b, a <=l b -> height a <= height b.
Proof. intros a b H. destruct a, b; simpl; try lia; inversion H. Qed.

Lemma height_le_2 : forall l, height l <= 2.
Proof. destruct l; simpl; lia. Qed.

Definition msr (P : prog) (L : nat -> lane) : nat :=
  fold_right (fun i acc => height (L i) + acc) 0 (seq 0 (length P)).

Lemma msr_gen_mono :
  forall (L M : nat -> lane) xs,
    (forall i, L i <=l M i) ->
    fold_right (fun i acc => height (L i) + acc) 0 xs
      <= fold_right (fun i acc => height (M i) + acc) 0 xs.
Proof.
  intros L M xs Hpt. induction xs as [| x xs IH]; simpl; [ lia | ].
  specialize (height_mono _ _ (Hpt x)). lia.
Qed.

Lemma msr_gen_eq_pointwise :
  forall (L M : nat -> lane) xs,
    (forall i, L i <=l M i) ->
    fold_right (fun i acc => height (L i) + acc) 0 xs
      = fold_right (fun i acc => height (M i) + acc) 0 xs ->
    forall i, In i xs -> L i = M i.
Proof.
  intros L M xs Hpt. induction xs as [| x xs IH]; simpl; intros Heq i Hin; [ contradiction | ].
  specialize (height_mono _ _ (Hpt x)) as Hx.
  specialize (msr_gen_mono L M xs Hpt) as Hrest.

  assert (Hhx : height (L x) = height (M x)) by lia.
  assert (Htail : fold_right (fun j acc => height (L j) + acc) 0 xs
                = fold_right (fun j acc => height (M j) + acc) 0 xs) by lia.
  destruct Hin as [-> | Hin].
  -
    specialize (Hpt i). destruct (L i), (M i); simpl in Hhx;
      try reflexivity; try discriminate; try inversion Hpt.
  - apply IH; assumption.
Qed.

Lemma msr_bound : forall P L, msr P L <= 2 * length P.
Proof.
  intros P L. unfold msr.
  assert (H : forall xs, fold_right (fun i acc => height (L i) + acc) 0 xs <= 2 * length xs).
  { induction xs as [| x xs IH]; simpl; [ lia | ]. specialize (height_le_2 (L x)). lia. }
  specialize (H (seq 0 (length P))). rewrite length_seq in H. exact H.
Qed.

Lemma msr_bottom : forall P L, (forall i, L i = Exact) -> msr P L = 0.
Proof.
  intros P L HL. unfold msr.
  assert (H : forall xs, fold_right (fun i acc => height (L i) + acc) 0 xs = 0).
  { induction xs as [| x xs IH]; simpl; [ reflexivity | rewrite HL, IH; reflexivity ]. }
  apply H.
Qed.

Lemma nondecreasing_plateau :
  forall (f : nat -> nat) N,
    (forall k, f k <= f (S k)) ->
    f N < f 0 + N ->
    exists k, k < N /\ f k = f (S k).
Proof.
  intros f N Hmono. induction N as [| N IH]; intros Hlt; [ lia | ].
  destruct (Nat.eq_dec (f N) (f (S N))) as [Heq | Hne].
  - exists N. split; [ lia | exact Heq ].
  - assert (f N < f (S N)) by (specialize (Hmono N); lia).
    destruct (IH ltac:(lia)) as [k [Hk Hfk]].
    exists k. split; [ lia | exact Hfk ].
Qed.

Theorem iter_converges :
  forall P, exists k, k <= 2 * length P /\ forall i, iter P (S k) i = iter P k i.
Proof.
  intros P.
  destruct (nondecreasing_plateau (fun k => msr P (iter P k)) (2 * length P + 1))
    as [k [Hk Heq]].
  - intros k. apply msr_gen_mono. intros i. apply iter_ascending.
  - rewrite (msr_bottom P (iter P 0) (fun _ => eq_refl)).
    specialize (msr_bound P (iter P (2 * length P + 1))). lia.
  - exists k. split; [ lia | ]. intros i.
    destruct (le_lt_dec (length P) i) as [Hge | Hlt].
    +
      rewrite !iter_oob by exact Hge. reflexivity.
    + symmetry.
      apply (msr_gen_eq_pointwise (iter P k) (iter P (S k)) (seq 0 (length P))).
      * intros j. apply iter_ascending.
      * exact Heq.
      * apply in_seq. lia.
Qed.

Theorem iter_le_derivable :
  forall P D,
    derivable D P ->
    forall k i, iter P k i <=l D i.
Proof.
  intros P D Hder k. induction k as [| k IH]; intros i.
  - apply exact_bottom.
  - simpl. unfold step. destruct (nth_error P i) as [n |] eqn:Hn.
    + eapply le_trans; [| apply (Hder i n Hn) ].
      apply node_lane_mono. exact IH.
    + apply exact_bottom.
Qed.

Lemma sound_is_derivable :
  forall P L, sound_assignment P L -> derivable L P.
Proof.
  intros P L Hsound i n Hnth.
  rewrite <- (Hsound i n Hnth). apply le_refl.
Qed.

Lemma stable_is_sound :
  forall P k,
    (forall i, iter P (S k) i = iter P k i) ->
    sound_assignment P (iter P k).
Proof.
  intros P k Hstab i n Hnth.
  rewrite <- (Hstab i). simpl. unfold step. rewrite Hnth. reflexivity.
Qed.

Theorem lemma1_least_fixed_point :
  forall P,
    exists L,
      sound_assignment P L /\
      (forall M, sound_assignment P M -> forall i, L i <=l M i).
Proof.
  intros P.
  destruct (iter_converges P) as [k [_ Hstab]].
  exists (iter P k). split.
  - apply stable_is_sound, Hstab.
  - intros M HM i. apply iter_le_derivable, sound_is_derivable, HM.
Qed.

Definition accepted (L : nat -> lane) (n : node) (i : nat) (d : lane) : Prop :=
  declared n = Some d /\ L i <=l d.

Corollary declaration_sound :
  forall P L,
    wf P ->
    Assumption1 P ->
    sound_assignment P L ->
    forall i n d s s' w,
      nth_error P i = Some n ->
      accepted L n i d ->
      carried_agrees P L s s' w ->
      agrees d s s' ->
      eval P i s w = eval P i s' w.
Proof.
  intros P L Hwf HA1 Hsound i n d s s' w Hnth [Hdecl Hacc] Hcar Hag.
  apply (theorem1 P L Hwf HA1 Hsound i n s s' w Hnth Hcar).
  eapply agrees_antitone; [ exact Hacc | exact Hag ].
Qed.
